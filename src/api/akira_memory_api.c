#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include "akira_api.h"
#include "akira_memory_api.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <runtime/security.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/slist.h>
#include <wasm_export.h>
#include <lib/mem_helper.h>
#include <akira_app_containment.h>
#include "akira_runtime.h"

LOG_MODULE_REGISTER(akira_memory_api, CONFIG_AKIRA_LOG_LEVEL);

extern akira_managed_app_t g_apps[AKIRA_MAX_WASM_INSTANCES];

extern int get_slot_for_module_inst(wasm_module_inst_t inst);

/* Largest single allocation, either kind (sanity limit). */
#define WASM_ALLOC_MAX (16u * 1024u * 1024u)

/* Charge @p bytes to @p app's quota (0 = unlimited) in one step, so two
 * allocations can't both slip under it. */
static bool quota_take(akira_managed_app_t *app, uint32_t bytes)
{
    atomic_val_t cur;

    do {
        cur = atomic_get(&app->memory_used);
        if (app->memory_quota > 0 &&
            (uint64_t)(uint32_t)cur + bytes > app->memory_quota) {
            return false;
        }
    } while (!atomic_cas(&app->memory_used, cur, cur + (atomic_val_t)bytes));
    return true;
}

static void quota_give(akira_managed_app_t *app, uint32_t bytes)
{
    atomic_val_t cur;

    do {
        cur = atomic_get(&app->memory_used);
    } while (!atomic_cas(&app->memory_used, cur,
                         (cur > (atomic_val_t)bytes) ? cur - (atomic_val_t)bytes : 0));
}

/*
 * Host memory allocated on behalf of an app (akira_wasm_malloc). Each block
 * is listed under its app's slot, so the release hook frees whatever the app
 * leaves behind, and akira_wasm_free() only frees blocks it finds in a list
 * (a forged or stale pointer is never dereferenced).
 */
#define AKIRA_ALLOC_MAGIC 0xAA4B5241u

typedef struct __aligned(8) {
    sys_snode_t node;     /* in s_host_blocks[slot] */
    uint32_t    magic;
    uint32_t    size;     /* bytes charged to the quota, header included */
    int32_t     app_slot;
} akira_alloc_header_t;

static sys_slist_t s_host_blocks[AKIRA_MAX_WASM_INSTANCES];
static struct k_spinlock s_host_lock;

/**
 * @brief Allocate memory for a WASM app with quota enforcement
 *
 * Allocates from PSRAM; on boards with PSRAM it never falls back to internal
 * SRAM, which the OS needs. Enforces the per-app memory quota and returns
 * NULL on violation without crashing the system. Freed when the app ends.
 *
 * @param exec_env  WAMR execution environment (identifies the app)
 * @param size      Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure/quota exceeded
 */
void *akira_wasm_malloc(wasm_exec_env_t exec_env, size_t size)
{
    if (!exec_env || size == 0 || size > WASM_ALLOC_MAX) {
        return NULL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    int slot = inst ? get_slot_for_module_inst(inst) : -1;
    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        LOG_WRN_RATELIMIT("wasm_malloc: unknown app instance");
        return NULL;
    }

    akira_managed_app_t *app = &g_apps[slot];
    uint32_t total = (uint32_t)size + (uint32_t)sizeof(akira_alloc_header_t);

    if (!quota_take(app, total)) {
        LOG_WRN_RATELIMIT("wasm_malloc: quota exceeded for app %s (req=%u, quota=%u)",
                          app->name, total, app->memory_quota);
        return NULL;  /* Graceful failure - no crash */
    }

    /* PSRAM only on boards that have it: never the OS's internal SRAM. */
    akira_alloc_header_t *hdr = akira_malloc_app_buffer(total);
    if (!hdr) {
        quota_give(app, total);
        LOG_WRN_RATELIMIT("wasm_malloc: allocation failed for %u bytes", total);
        return NULL;
    }

    hdr->magic = AKIRA_ALLOC_MAGIC;
    hdr->size = total;
    hdr->app_slot = slot;

    k_spinlock_key_t key = k_spin_lock(&s_host_lock);
    sys_slist_append(&s_host_blocks[slot], &hdr->node);
    k_spin_unlock(&s_host_lock, key);

    LOG_DBG("wasm_malloc: app %s allocated %zu bytes (total used: %ld)",
            app->name, size, (long)atomic_get(&app->memory_used));

    /* Return pointer past header */
    return (void *)(hdr + 1);
}

/**
 * @brief Free memory allocated with akira_wasm_malloc
 *
 * Updates quota tracking and frees the underlying memory. Safe to call with
 * NULL, a pointer it did not allocate, or one already freed (both refused).
 *
 * @param exec_env  WAMR execution environment
 * @param ptr       Pointer to memory (from akira_wasm_malloc)
 */
void akira_wasm_free(wasm_exec_env_t exec_env, void *ptr)
{
    ARG_UNUSED(exec_env);

    if (!ptr) {
        return;
    }

    akira_alloc_header_t *hdr = ((akira_alloc_header_t *)ptr) - 1;
    bool found = false;
    k_spinlock_key_t key = k_spin_lock(&s_host_lock);

    for (int s = 0; s < AKIRA_MAX_WASM_INSTANCES && !found; s++) {
        found = sys_slist_find_and_remove(&s_host_blocks[s], &hdr->node);
    }
    k_spin_unlock(&s_host_lock, key);

    if (!found) {
        LOG_ERR_RATELIMIT("wasm_free: %p was not allocated by wasm_malloc (or already freed)", ptr);
        return;  /* Don't crash - graceful failure */
    }

    quota_give(&g_apps[hdr->app_slot], hdr->size);
    LOG_DBG("wasm_free: app %s freed %u bytes (remaining: %ld)",
            g_apps[hdr->app_slot].name, hdr->size,
            (long)atomic_get(&g_apps[hdr->app_slot].memory_used));

    hdr->magic = 0;
    akira_free_buffer(hdr);
}

/* The app is gone: free every host block it still had. Runs late (order 90),
 * after the subsystems that might still hold pointers into them. */
static void wasm_malloc_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);

    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        return;
    }
    for (;;) {
        k_spinlock_key_t key = k_spin_lock(&s_host_lock);
        sys_snode_t *node = sys_slist_get(&s_host_blocks[slot]);

        k_spin_unlock(&s_host_lock, key);
        if (!node) {
            return;
        }

        akira_alloc_header_t *hdr = CONTAINER_OF(node, akira_alloc_header_t, node);

        quota_give(&g_apps[slot], hdr->size);
        hdr->magic = 0;
        akira_free_buffer(hdr);
    }
}

AKIRA_APP_RESOURCE_DEFINE(wasm_malloc, 90, NULL, wasm_malloc_release);

/* WASM Native export api
 *
 * mem_alloc hands out memory from the app's own WAMR heap (inside its linear
 * memory, already bounded at instantiation, freed with the instance) —
 * nothing is taken from the system heaps. Each block carries an 8-byte header
 * in front of the returned pointer: [magic ^ header offset][size], so
 * mem_free returns exactly what mem_alloc charged to the app's quota and
 * refuses pointers it never handed out or already freed. The header lives in
 * app memory: an app that forges one can only mis-account its own quota
 * inside its own fixed heap. */

#define MEM_ALLOC_MAGIC 0xA4C1A110u

typedef struct {
    uint32_t tag;  /* MEM_ALLOC_MAGIC ^ app offset of this header */
    uint32_t size; /* bytes charged to the quota, header included */
} mem_block_hdr_t;

uint32_t akira_native_mem_alloc(wasm_exec_env_t exec_env, uint32_t size)
{
    /* Capability gate: only apps that declared "memory" in their manifest
     * are allowed to call mem_alloc. Quota is still enforced on top of this. */
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MEMORY, 0);

    if (size == 0 || size > WASM_ALLOC_MAX) {
        return 0;
    }

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    int slot = module_inst ? get_slot_for_module_inst(module_inst) : -1;
    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        return 0;
    }
    akira_managed_app_t *app = &g_apps[slot];

    /* Allocate size + header so free() can recover the block size. */
    uint32_t total = size + (uint32_t)sizeof(mem_block_hdr_t);

    /* Enforce the per-app quota before allocating (quota == 0 => unlimited). */
    if (!quota_take(app, total)) {
        LOG_WRN_RATELIMIT("mem_alloc: quota exceeded for app %s (req=%u, quota=%u)",
                          app->name, total, app->memory_quota);
        return 0;
    }

    /* May run the app's own malloc, and that may grow (move) its memory:
     * the header is located afresh below. */
    uint64_t hdr_off = wasm_runtime_module_malloc(module_inst, total, NULL);
    if (hdr_off == 0 || hdr_off > (uint64_t)UINT32_MAX - total ||
        !wasm_runtime_validate_app_addr(module_inst, hdr_off, total)) {
        if (hdr_off != 0) {
            wasm_runtime_module_free(module_inst, hdr_off);
        }
        quota_give(app, total);
        LOG_WRN_RATELIMIT("mem_alloc: WAMR module malloc failed for %u bytes", total);
        return 0;
    }

    mem_block_hdr_t hdr = { .tag = MEM_ALLOC_MAGIC ^ (uint32_t)hdr_off, .size = total };
    memcpy(wasm_runtime_addr_app_to_native(module_inst, hdr_off), &hdr, sizeof(hdr));

    LOG_DBG("mem_alloc: app %s allocated %u bytes (used: %ld)",
            app->name, size, (long)atomic_get(&app->memory_used));

    /* Hand the app the region just past the header. */
    return (uint32_t)hdr_off + (uint32_t)sizeof(mem_block_hdr_t);
}

void akira_native_mem_free(wasm_exec_env_t exec_env, uint32_t ptr)
{
    /* Capability gate: only apps that declared "memory" can call mem_free.
     * This prevents a rogue app from freeing pointers obtained by other means. */
    if (!akira_security_check_exec(exec_env, AKIRA_CAP_MEMORY)) {
        LOG_WRN_RATELIMIT("mem_free: capability denied");
        return;
    }

    if (ptr < sizeof(mem_block_hdr_t)) {
        return; /* NULL, or not something mem_alloc returned */
    }

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return;
    }

    /* Recover the header written by mem_alloc(), validating the range so a
     * forged pointer cannot cause an OOB read. */
    uint32_t hdr_off = ptr - (uint32_t)sizeof(mem_block_hdr_t);
    if (!wasm_runtime_validate_app_addr(module_inst, hdr_off, sizeof(mem_block_hdr_t))) {
        LOG_WRN_RATELIMIT("mem_free: invalid pointer 0x%08x", ptr);
        return;
    }
    void *native = wasm_runtime_addr_app_to_native(module_inst, hdr_off);

    mem_block_hdr_t hdr;
    memcpy(&hdr, native, sizeof(hdr));
    if (hdr.tag != (MEM_ALLOC_MAGIC ^ hdr_off) || hdr.size < sizeof(hdr) ||
        hdr.size > WASM_ALLOC_MAX + sizeof(hdr)) {
        LOG_WRN_RATELIMIT("mem_free: 0x%08x was not returned by mem_alloc (or already freed)",
                          ptr);
        return;
    }

    /* Kill the tag first: a second free of the same pointer is refused. */
    memset(native, 0, sizeof(hdr));

    /* Return the block's bytes to the app's quota. */
    int slot = get_slot_for_module_inst(module_inst);
    if (slot >= 0 && slot < AKIRA_MAX_WASM_INSTANCES) {
        quota_give(&g_apps[slot], hdr.size);
        LOG_DBG("mem_free: app %s freed %u bytes (remaining: %ld)",
                g_apps[slot].name, hdr.size, (long)atomic_get(&g_apps[slot].memory_used));
    }

    wasm_runtime_module_free(module_inst, hdr_off);
}
#endif

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_MEMORY))
#include <akira_native_registry.h>

static const NativeSymbol akira_memory_natives[] = {
    {"mem_alloc", (void *)akira_native_mem_alloc, "(i)i", NULL},
    {"mem_free", (void *)akira_native_mem_free, "(i)", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_memory_api, "env", akira_memory_natives);
#endif
