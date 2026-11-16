/**
 * @file sandbox.c
 * @brief AkiraOS Runtime Sandbox Implementation
 *
 * Implements syscall filtering, rate limiting, execution watchdog,
 * and security audit logging for WASM application sandboxing.
 *
 * Performance targets:
 * - Syscall check: <100ns (bitmask + atomic decrement)
 * - Rate limit refill: amortized, runs on slow path
 * - Audit log: lock-free ring buffer write
 */

#include "sandbox.h"
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include "runtime/security.h"
#include <lib/mem_helper.h>
#include <akira_audit.h>

BUILD_ASSERT((int)AUDIT_EVENT_SYSCALL_DENIED == (int)AKIRA_AUDIT_SYSCALL_DENIED &&
             (int)AUDIT_EVENT_APP_STOPPED == (int)AKIRA_AUDIT_APP_STOPPED &&
             (int)AUDIT_EVENT_ADMISSION_DENIED == (int)AKIRA_AUDIT_ADMISSION_DENIED &&
             (int)AUDIT_EVENT_NET_DENIED == (int)AKIRA_AUDIT_NET_DENIED,
             "audit_event_type_t must match enum akira_audit_type");
BUILD_ASSERT(sizeof(((audit_entry_t *)0)->app_name) == AKIRA_AUDIT_APP_NAME_LEN);

LOG_MODULE_REGISTER(akira_sandbox, CONFIG_AKIRA_LOG_LEVEL);

/* ===== Audit Ring Buffer ===== */

/* The ring buffer is the largest static in this file. Place it in PSRAM
 * (.ext_ram.bss) on boards that have it, keeping it out of scarce internal
 * DRAM; it is only written on the slow path (syscall-deny / rate-limit /
 * watchdog events), never on the <100 ns syscall hot path. Zeroed at boot
 * like any BSS. AKIRA_BULK_BSS is a no-op on non-PSRAM boards. */
static struct
{
    audit_entry_t entries[CONFIG_AKIRA_AUDIT_LOG_SIZE];
    atomic_t write_idx;
    atomic_t count;
    bool initialized;
} g_audit AKIRA_BULK_BSS;

static struct k_spinlock g_audit_lock;

/* ===== Audit sinks (akira_audit.h) =====
 * Records are delivered from the system workqueue, outside g_audit_lock, in
 * sequence order. g_sink_next is the seq of the next record to deliver; the
 * writer never waits for sinks, so a slow sink sees an AKIRA_AUDIT_DROPPED
 * record when the ring overtook it. */
static struct k_work g_audit_sink_work;
static uint32_t g_sink_next;
static bool g_have_sinks;

static void audit_sinks_deliver(const struct akira_audit_record *rec)
{
    STRUCT_SECTION_FOREACH(akira_audit_sink, sink) {
        if (sink->fn) {
            sink->fn(rec, sink->user_data);
        }
    }
}

static void audit_sink_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    for (;;) {
        struct akira_audit_record rec;
        uint32_t dropped = 0;
        k_spinlock_key_t key = k_spin_lock(&g_audit_lock);
        uint32_t head = (uint32_t)atomic_get(&g_audit.write_idx);

        if (g_sink_next == head) {
            k_spin_unlock(&g_audit_lock, key);
            return;
        }
        if (head - g_sink_next > CONFIG_AKIRA_AUDIT_LOG_SIZE) {
            dropped = head - g_sink_next - CONFIG_AKIRA_AUDIT_LOG_SIZE;
            memset(&rec, 0, sizeof(rec));
            rec.seq = g_sink_next;
            rec.type = AKIRA_AUDIT_DROPPED;
            rec.timestamp_ms = k_uptime_get();
            rec.detail = dropped;
            g_sink_next = head - CONFIG_AKIRA_AUDIT_LOG_SIZE;
        } else {
            const audit_entry_t *e =
                &g_audit.entries[g_sink_next % CONFIG_AKIRA_AUDIT_LOG_SIZE];

            rec.seq = e->seq;
            rec.type = e->type;
            rec.timestamp_ms = e->timestamp_ms;
            rec.detail = e->detail;
            memcpy(rec.app_name, e->app_name, sizeof(rec.app_name));
            g_sink_next++;
        }
        k_spin_unlock(&g_audit_lock, key);

        audit_sinks_deliver(&rec);
    }
}

/* ===== Rate Limit Helpers ===== */

/**
 * @brief Refill rate limit tokens based on elapsed time.
 *
 * Uses a CAS on last_refill_ms to ensure exactly one thread performs the
 * refill per 20 ms window, preventing double-refill races on dual-core SoCs.
 */
static void rate_bucket_refill(sandbox_rate_bucket_t *bucket)
{
    uint32_t now = k_uptime_get_32();
    atomic_val_t old_ms = atomic_get(&bucket->last_refill_ms);
    uint32_t elapsed_ms = now - (uint32_t)old_ms;

    if (elapsed_ms < 20)
    {
        return;
    }

    /* Claim this refill window: if another thread already updated
     * last_refill_ms, the CAS fails and we skip the refill. */
    if (!atomic_cas(&bucket->last_refill_ms, old_ms, (atomic_val_t)now))
    {
        return;
    }

    int32_t new_tokens = (int32_t)((elapsed_ms * bucket->refill_per_sec) / 1000);
    if (new_tokens > 0)
    {
        /* CAS loop: add tokens up to max_tokens without a plain read-modify-write race */
        int32_t old_tok, new_tok;
        do {
            old_tok = atomic_get(&bucket->tokens);
            new_tok = MIN(old_tok + new_tokens, (int32_t)bucket->max_tokens);
            if (new_tok == old_tok)
            {
                break; /* Already at max */
            }
        } while (!atomic_cas(&bucket->tokens, old_tok, new_tok));
    }
}

/**
 * @brief Try to consume a rate limit token.
 *
 * Uses a CAS loop so that concurrent callers cannot both succeed when only
 * one token remains, preventing the over-decrement that could eventually
 * wrap the counter past INT32_MIN and reset the limit.
 *
 * @return true if token consumed, false if rate limited
 */
static bool rate_bucket_try_consume(sandbox_rate_bucket_t *bucket)
{
    int32_t old;

    do {
        old = atomic_get(&bucket->tokens);
        if (old <= 0)
        {
            /* Slow path: try refill, then re-check */
            rate_bucket_refill(bucket);
            old = atomic_get(&bucket->tokens);
            if (old <= 0)
            {
                return false;
            }
        }
        /* CAS: decrement only if the value hasn't changed under us */
    } while (!atomic_cas(&bucket->tokens, old, old - 1));

    return true;
}

/**
 * @brief Map syscall category to rate bucket index
 * @return Bucket index, or -1 if no rate limiting for this category
 */
static int category_to_bucket(sandbox_syscall_cat_t cat)
{
    switch (cat)
    {
    case SYSCALL_CAT_DISPLAY:
        return 0;
    case SYSCALL_CAT_SENSOR:
        return 1;
    case SYSCALL_CAT_RF:
        return 2;
    case SYSCALL_CAT_NETWORK:
        return 3;
    case SYSCALL_CAT_STORAGE:
        return 4;
    case SYSCALL_CAT_IPC:
        return 5;
    default:
        return -1; /* No rate limit */
    }
}

/* ===== Sandbox API Implementation ===== */

int sandbox_init(void)
{
    memset(&g_audit, 0, sizeof(g_audit));
    atomic_set(&g_audit.write_idx, 0);
    atomic_set(&g_audit.count, 0);
    g_sink_next = 0;
    g_have_sinks = false;
    STRUCT_SECTION_FOREACH(akira_audit_sink, sink) {
        g_have_sinks = true;
    }
    k_work_init(&g_audit_sink_work, audit_sink_work_handler);
    g_audit.initialized = true;

    LOG_INF("Sandbox subsystem initialized (audit_log=%d entries)",
            CONFIG_AKIRA_AUDIT_LOG_SIZE);
    return 0;
}

void sandbox_ctx_init(sandbox_ctx_t *ctx, akira_trust_level_t trust,
                      uint64_t cap_mask)
{
    if (!ctx)
        return;

    memset(ctx, 0, sizeof(*ctx));
    ctx->trust_level = trust;

    /* Set allowed syscall categories based on trust level */
    switch (trust)
    {
    case TRUST_LEVEL_KERNEL:
        ctx->allowed_syscalls = SANDBOX_TRUST_KERNEL_ALLOWED;
        break;
    case TRUST_LEVEL_SYSTEM:
        ctx->allowed_syscalls = SANDBOX_TRUST_SYSTEM_ALLOWED;
        break;
    case TRUST_LEVEL_TRUSTED:
        ctx->allowed_syscalls = SANDBOX_TRUST_TRUSTED_ALLOWED;
        break;
    case TRUST_LEVEL_USER:
    default:
        ctx->allowed_syscalls = SANDBOX_TRUST_USER_ALLOWED;
        break;
    }

    /* Refine allowed syscalls based on capability mask.
     * If the app has specific capabilities granted via manifest,
     * expand the allowed syscalls accordingly.
     */
    if (cap_mask & AKIRA_CAP_DISPLAY_WRITE) /* DISPLAY_WRITE */
        ctx->allowed_syscalls |= SYSCALL_CAT_DISPLAY;
    if (cap_mask & AKIRA_CAP_INPUT_READ) /* INPUT_READ */
        ctx->allowed_syscalls |= SYSCALL_CAT_INPUT;
    if (cap_mask & AKIRA_CAP_SENSOR_READ) /* SENSOR_READ */
        ctx->allowed_syscalls |= SYSCALL_CAT_SENSOR;
    if (cap_mask & AKIRA_CAP_RF_TRANSCEIVE) /* RF_TRANSCEIVE */
        ctx->allowed_syscalls |= SYSCALL_CAT_RF;
    if (cap_mask & AKIRA_CAP_BLE) /* BLE app API */
        ctx->allowed_syscalls |= SYSCALL_CAT_BLUETOOTH;
    if (cap_mask & (AKIRA_CAP_BLE_SCAN | AKIRA_CAP_BLE_SPAM))
        ctx->allowed_syscalls |= SYSCALL_CAT_BLUETOOTH;

    /* Initialize rate limit buckets */
    static const uint16_t bucket_rates[] = {
        SANDBOX_RATE_DISPLAY_OPS,
        SANDBOX_RATE_SENSOR_READS,
        SANDBOX_RATE_RF_OPS,
        SANDBOX_RATE_NETWORK_OPS,
        SANDBOX_RATE_STORAGE_OPS,
        SANDBOX_RATE_IPC_OPS,
    };

    uint32_t now = k_uptime_get_32();
    for (int i = 0; i < SANDBOX_NUM_RATE_BUCKETS; i++)
    {
        ctx->rate_buckets[i].max_tokens = bucket_rates[i];
        ctx->rate_buckets[i].refill_per_sec = bucket_rates[i];
        atomic_set(&ctx->rate_buckets[i].tokens, bucket_rates[i]);
        atomic_set(&ctx->rate_buckets[i].last_refill_ms, (atomic_val_t)now);
    }

    /* Execution watchdog */
    ctx->exec_timeout_ms = CONFIG_AKIRA_SANDBOX_EXEC_TIMEOUT_MS;
    ctx->exec_active = false;

    ctx->initialized = true;
}

bool sandbox_check_syscall(sandbox_ctx_t *ctx, sandbox_syscall_cat_t category,
                           const char *app_name)
{
    if (!ctx || !ctx->initialized)
        return false;

    ctx->total_syscalls++;

    /* Fast path: category filter (single AND + branch) */
    if ((ctx->allowed_syscalls & category) == 0)
    {
        ctx->denied_syscalls++;
        sandbox_audit_log(AUDIT_EVENT_SYSCALL_DENIED,
                          app_name ? app_name : "unknown", (uint32_t)category);
        return false;
    }

    /* Rate limit check */
    int bucket_idx = category_to_bucket(category);
    if (bucket_idx >= 0)
    {
        if (!rate_bucket_try_consume(&ctx->rate_buckets[bucket_idx]))
        {
            ctx->rate_limited_count++;
            sandbox_audit_log(AUDIT_EVENT_RATE_LIMITED,
                              app_name ? app_name : "unknown", (uint32_t)category);
            return false;
        }
    }

    return true;
}

void sandbox_exec_begin(sandbox_ctx_t *ctx)
{
    if (!ctx)
        return;
    ctx->exec_start_ms = k_uptime_get();
    ctx->exec_active = true;

#ifdef CONFIG_AKIRA_SANDBOX_MPU
    if (ctx->mpu_ready) {
        k_mem_domain_add_thread(&ctx->mpu_domain, k_current_get());
    }
#endif
}

void sandbox_exec_end(sandbox_ctx_t *ctx)
{
    if (!ctx)
        return;
    ctx->exec_active = false;

#ifdef CONFIG_AKIRA_SANDBOX_MPU
    if (ctx->mpu_ready) {
        /* Remove thread from restricted domain, returning it to the default
         * kernel domain which has access to all kernel memory. */
        k_mem_domain_remove_thread(k_current_get());
    }
#endif
}

#ifdef CONFIG_AKIRA_SANDBOX_MPU
void sandbox_mpu_configure(sandbox_ctx_t *ctx, void *wasm_heap, size_t heap_size)
{
    if (!ctx || !wasm_heap || heap_size == 0)
        return;

    ctx->mpu_wasm_partition = (struct k_mem_partition){
        .start = (uintptr_t)wasm_heap,
        .size  = heap_size,
        .attr  = K_MEM_PARTITION_P_RW_U_RW,
    };

    struct k_mem_partition *parts[] = { &ctx->mpu_wasm_partition };
    k_mem_domain_init(&ctx->mpu_domain, ARRAY_SIZE(parts), parts);
    ctx->mpu_ready = true;

    LOG_INF("Sandbox MPU region configured: base=0x%08x size=%zu",
            (unsigned)wasm_heap, heap_size);
}
#endif /* CONFIG_AKIRA_SANDBOX_MPU */

bool sandbox_exec_timed_out(sandbox_ctx_t *ctx)
{
    if (!ctx || !ctx->exec_active)
        return false;

    int64_t elapsed = k_uptime_get() - ctx->exec_start_ms;
    return (elapsed > (int64_t)ctx->exec_timeout_ms);
}

void sandbox_watchdog_kill(sandbox_ctx_t *ctx, const char *app_name)
{
    if (!ctx)
        return;

    ctx->exec_active = false;
    ctx->watchdog_kills++;

    sandbox_audit_log(AUDIT_EVENT_WATCHDOG_KILL,
                      app_name ? app_name : "unknown",
                      ctx->exec_timeout_ms);
}

void sandbox_audit_log(audit_event_type_t type, const char *app_name,
                       uint32_t detail)
{
    akira_audit_emit((uint32_t)type, app_name, detail);
}

void akira_audit_emit(uint32_t type, const char *app_name, uint32_t detail)
{
    if (!g_audit.initialized)
        return;

    /* Serialize the slot claim + field writes under a spinlock.
     * Without this, two concurrent writers that claim adjacent slots could
     * still race if the ring wraps and they end up at the same slot, and the
     * reader can see a partially-written entry.  The spinlock is ISR-safe and
     * held only for the duration of one entry write (~100 ns). */
    k_spinlock_key_t key = k_spin_lock(&g_audit_lock);

    uint32_t seq = (uint32_t)atomic_inc(&g_audit.write_idx);
    int idx = (int)(seq % CONFIG_AKIRA_AUDIT_LOG_SIZE);

    audit_entry_t *entry = &g_audit.entries[idx];
    entry->type = type;
    entry->seq = seq;
    entry->timestamp_ms = k_uptime_get();
    entry->detail = detail;

    if (app_name)
    {
        strncpy(entry->app_name, app_name, sizeof(entry->app_name) - 1);
        entry->app_name[sizeof(entry->app_name) - 1] = '\0';
    }
    else
    {
        entry->app_name[0] = '\0';
    }


    int32_t count = atomic_get(&g_audit.count);
    if (count < CONFIG_AKIRA_AUDIT_LOG_SIZE)
    {
        atomic_inc(&g_audit.count);
    }

    k_spin_unlock(&g_audit_lock, key);

    if (g_have_sinks) {
        k_work_submit(&g_audit_sink_work);
    }

    /* Log critical security events outside the lock to minimise hold time */
    if (type == AUDIT_EVENT_SYSCALL_DENIED || type == AUDIT_EVENT_WATCHDOG_KILL ||
        type == AUDIT_EVENT_INTEGRITY_FAIL || type == AUDIT_EVENT_SIGNATURE_FAIL ||
        type == AUDIT_EVENT_ADMISSION_DENIED || type == AUDIT_EVENT_NET_DENIED)
    {
        LOG_WRN("SECURITY [%s] event=%d detail=0x%08x",
                app_name ? app_name : "?", type, detail);
    }
}

int sandbox_audit_get_recent(audit_entry_t *entries, int max_count)
{
    if (!entries || max_count <= 0 || !g_audit.initialized)
        return 0;

    k_spinlock_key_t key = k_spin_lock(&g_audit_lock);

    int32_t total = atomic_get(&g_audit.count);
    int count = MIN(total, max_count);
    int32_t write_pos = atomic_get(&g_audit.write_idx);

    for (int i = 0; i < count; i++)
    {
        int src_idx = (write_pos - count + i + CONFIG_AKIRA_AUDIT_LOG_SIZE) % CONFIG_AKIRA_AUDIT_LOG_SIZE;
        memcpy(&entries[i], &g_audit.entries[src_idx], sizeof(audit_entry_t));
    }

    k_spin_unlock(&g_audit_lock, key);
    return count;
}

int sandbox_get_stats(const sandbox_ctx_t *ctx, char *buf, size_t len)
{
    if (!ctx || !buf || len == 0)
        return 0;

    return snprintf(buf, len,
                    "trust=%d syscalls=%u denied=%u rate_limited=%u watchdog_kills=%u",
                    ctx->trust_level, ctx->total_syscalls, ctx->denied_syscalls,
                    ctx->rate_limited_count, ctx->watchdog_kills);
}
