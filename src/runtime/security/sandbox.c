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

LOG_MODULE_REGISTER(akira_sandbox, CONFIG_AKIRA_LOG_LEVEL);

/* ===== Audit Ring Buffer ===== */

static struct {
    audit_entry_t entries[CONFIG_AKIRA_AUDIT_LOG_SIZE];
    atomic_t write_idx;
    atomic_t count;
    bool initialized;
} g_audit = {0};

static K_MUTEX_DEFINE(g_audit_mutex);

/* ===== Rate Limit Helpers ===== */

/**
 * @brief Refill rate limit tokens based on elapsed time
 */
static void rate_bucket_refill(sandbox_rate_bucket_t *bucket)
{
    int64_t now = k_uptime_get();
    int64_t elapsed_ms = now - bucket->last_refill_ms;

    if (elapsed_ms < 20) {
        return; /* Refill at most every 20ms to reduce overhead */
    }

    /* Calculate tokens to add */
    int32_t new_tokens = (int32_t)((elapsed_ms * bucket->refill_per_sec) / 1000);
    if (new_tokens > 0) {
        int32_t current = atomic_get(&bucket->tokens);
        int32_t target = MIN(current + new_tokens, (int32_t)bucket->max_tokens);
        atomic_set(&bucket->tokens, target);
        bucket->last_refill_ms = now;
    }
}

/**
 * @brief Try to consume a rate limit token
 * @return true if token consumed, false if rate limited
 */
static bool rate_bucket_try_consume(sandbox_rate_bucket_t *bucket)
{
    /* Fast path: try atomic decrement */
    int32_t old = atomic_get(&bucket->tokens);
    if (old <= 0) {
        /* Slow path: try refill */
        rate_bucket_refill(bucket);
        old = atomic_get(&bucket->tokens);
        if (old <= 0) {
            return false;
        }
    }

    /* Consume one token */
    atomic_dec(&bucket->tokens);
    return true;
}

/**
 * @brief Map syscall category to rate bucket index
 * @return Bucket index, or -1 if no rate limiting for this category
 */
static int category_to_bucket(sandbox_syscall_cat_t cat)
{
    switch (cat) {
    case SYSCALL_CAT_DISPLAY:   return 0;
    case SYSCALL_CAT_SENSOR:    return 1;
    case SYSCALL_CAT_RF:        return 2;
    case SYSCALL_CAT_NETWORK:   return 3;
    case SYSCALL_CAT_STORAGE:   return 4;
    case SYSCALL_CAT_IPC:       return 5;
    default:                    return -1; /* No rate limit */
    }
}

/* ===== Sandbox API Implementation ===== */

int sandbox_init(void)
{
    memset(&g_audit, 0, sizeof(g_audit));
    atomic_set(&g_audit.write_idx, 0);
    atomic_set(&g_audit.count, 0);
    g_audit.initialized = true;

    LOG_INF("Sandbox subsystem initialized (audit_log=%d entries)",
            CONFIG_AKIRA_AUDIT_LOG_SIZE);
    return 0;
}

void sandbox_ctx_init(sandbox_ctx_t *ctx, akira_trust_level_t trust,
                      uint32_t cap_mask)
{
    if (!ctx) return;

    memset(ctx, 0, sizeof(*ctx));
    ctx->trust_level = trust;

    /* Set allowed syscall categories based on trust level */
    switch (trust) {
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
    if (cap_mask & (1U << 0)) /* DISPLAY_WRITE */
        ctx->allowed_syscalls |= SYSCALL_CAT_DISPLAY;
    if (cap_mask & (1U << 1)) /* INPUT_READ */
        ctx->allowed_syscalls |= SYSCALL_CAT_INPUT;
    if (cap_mask & (1U << 3)) /* SENSOR_READ */
        ctx->allowed_syscalls |= SYSCALL_CAT_SENSOR;
    if (cap_mask & (1U << 4)) /* RF_TRANSCEIVE */
        ctx->allowed_syscalls |= SYSCALL_CAT_RF;
    if (cap_mask & (1U << 5)) /* BT_SHELL */
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

    int64_t now = k_uptime_get();
    for (int i = 0; i < SANDBOX_NUM_RATE_BUCKETS; i++) {
        ctx->rate_buckets[i].max_tokens = bucket_rates[i];
        ctx->rate_buckets[i].refill_per_sec = bucket_rates[i];
        atomic_set(&ctx->rate_buckets[i].tokens, bucket_rates[i]);
        ctx->rate_buckets[i].last_refill_ms = now;
    }

    /* Execution watchdog */
    ctx->exec_timeout_ms = CONFIG_AKIRA_SANDBOX_EXEC_TIMEOUT_MS;
    ctx->exec_active = false;

    ctx->initialized = true;
}

bool sandbox_check_syscall(sandbox_ctx_t *ctx, sandbox_syscall_cat_t category,
                           const char *app_name)
{
    if (!ctx || !ctx->initialized) return false;

    ctx->total_syscalls++;

    /* Fast path: category filter (single AND + branch) */
    if ((ctx->allowed_syscalls & category) == 0) {
        ctx->denied_syscalls++;
        sandbox_audit_log(AUDIT_EVENT_SYSCALL_DENIED,
                          app_name ? app_name : "unknown", (uint32_t)category);
        return false;
    }

    /* Rate limit check */
    int bucket_idx = category_to_bucket(category);
    if (bucket_idx >= 0) {
        if (!rate_bucket_try_consume(&ctx->rate_buckets[bucket_idx])) {
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
    if (!ctx) return;
    ctx->exec_start_ms = k_uptime_get();
    ctx->exec_active = true;
}

void sandbox_exec_end(sandbox_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->exec_active = false;
}

bool sandbox_exec_timed_out(sandbox_ctx_t *ctx)
{
    if (!ctx || !ctx->exec_active) return false;

    int64_t elapsed = k_uptime_get() - ctx->exec_start_ms;
    return (elapsed > (int64_t)ctx->exec_timeout_ms);
}

void sandbox_audit_log(audit_event_type_t type, const char *app_name,
                       uint32_t detail)
{
    if (!g_audit.initialized) return;

    /* Lock-free ring buffer write */
    int idx = atomic_inc(&g_audit.write_idx) % CONFIG_AKIRA_AUDIT_LOG_SIZE;

    audit_entry_t *entry = &g_audit.entries[idx];
    entry->type = type;
    entry->timestamp_ms = k_uptime_get();
    entry->detail = detail;

    if (app_name) {
        strncpy(entry->app_name, app_name, sizeof(entry->app_name) - 1);
        entry->app_name[sizeof(entry->app_name) - 1] = '\0';
    } else {
        entry->app_name[0] = '\0';
    }

    int32_t count = atomic_get(&g_audit.count);
    if (count < CONFIG_AKIRA_AUDIT_LOG_SIZE) {
        atomic_inc(&g_audit.count);
    }

    /* Log critical security events */
    if (type == AUDIT_EVENT_SYSCALL_DENIED || type == AUDIT_EVENT_WATCHDOG_KILL ||
        type == AUDIT_EVENT_INTEGRITY_FAIL || type == AUDIT_EVENT_SIGNATURE_FAIL) {
        LOG_WRN("SECURITY [%s] event=%d detail=0x%08x",
                app_name ? app_name : "?", type, detail);
    }
}

int sandbox_audit_get_recent(audit_entry_t *entries, int max_count)
{
    if (!entries || max_count <= 0 || !g_audit.initialized) return 0;

    k_mutex_lock(&g_audit_mutex, K_FOREVER);

    int32_t total = atomic_get(&g_audit.count);
    int count = MIN(total, max_count);
    int32_t write_pos = atomic_get(&g_audit.write_idx);

    for (int i = 0; i < count; i++) {
        int src_idx = (write_pos - count + i + CONFIG_AKIRA_AUDIT_LOG_SIZE)
                      % CONFIG_AKIRA_AUDIT_LOG_SIZE;
        memcpy(&entries[i], &g_audit.entries[src_idx], sizeof(audit_entry_t));
    }

    k_mutex_unlock(&g_audit_mutex);
    return count;
}

int sandbox_get_stats(const sandbox_ctx_t *ctx, char *buf, size_t len)
{
    if (!ctx || !buf || len == 0) return 0;

    return snprintf(buf, len,
        "trust=%d syscalls=%u denied=%u rate_limited=%u watchdog_kills=%u",
        ctx->trust_level, ctx->total_syscalls, ctx->denied_syscalls,
        ctx->rate_limited_count, ctx->watchdog_kills);
}
