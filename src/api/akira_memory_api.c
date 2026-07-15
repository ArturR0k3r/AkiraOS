#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include "akira_api.h"
#include "akira_memory_api.h"
#include <stdint.h>
#include <stddef.h>
#include <runtime/security.h>
#include <zephyr/logging/log.h>
#include <wasm_export.h>
#include <lib/mem_helper.h>
#include "akira_runtime.h"

LOG_MODULE_REGISTER(akira_memory_api, CONFIG_AKIRA_LOG_LEVEL);

extern akira_managed_app_t g_apps[AKIRA_MAX_WASM_INSTANCES];

extern int get_slot_for_module_inst(wasm_module_inst_t inst);

/**
 * @brief Allocate memory for a WASM app with quota enforcement
 *
 * Attempts to allocate from PSRAM first, falls back to SRAM.
 * Enforces per-app memory quota. Returns NULL on quota violation
 * without crashing the system.
 *
 * @param exec_env  WAMR execution environment (identifies the app)
 * @param size      Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure/quota exceeded
 */
void *akira_wasm_malloc(wasm_exec_env_t exec_env, size_t size)
{
    if (!exec_env || size == 0) {
        return NULL;
    }

#ifdef CONFIG_AKIRA_WASM_RUNTIME
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if(!inst) {
        LOG_WRN("wasm_malloc: invalid exec_env");
        return NULL;
    }
    int slot = get_slot_for_module_inst(inst);
    if (slot < 0) {
        LOG_WRN("wasm_malloc: unknown app instance");
        return NULL;
    }

    akira_managed_app_t *app = &g_apps[slot];

    /* Calculate total allocation size including header */
    size_t total_size = size + sizeof(akira_alloc_header_t);

    /* Check quota before allocation (atomic read) */
    uint32_t quota = app->memory_quota;
    if (quota > 0) {
        atomic_val_t current = atomic_get(&app->memory_used);
        if ((uint32_t)current + total_size > quota) {
            LOG_WRN("wasm_malloc: quota exceeded for app %s (used=%ld, req=%zu, quota=%u)",
                    app->name, current, total_size, quota);
            return NULL;  /* Graceful failure - no crash */
        }
    }

    /* Allocate from PSRAM-preferred pool */
    void *raw = akira_malloc_buffer(total_size);
    if (!raw) {
        LOG_WRN("wasm_malloc: allocation failed for %zu bytes", total_size);
        return NULL;
    }

    /* Initialize header */
    akira_alloc_header_t *hdr = (akira_alloc_header_t *)raw;
    hdr->magic = AKIRA_ALLOC_MAGIC;
    hdr->size = (uint32_t)size;
    hdr->app_slot = slot;

    /* Update quota tracking */
    atomic_add(&app->memory_used, (atomic_val_t)total_size);

    LOG_DBG("wasm_malloc: app %s allocated %zu bytes (total used: %ld)",
            app->name, size, atomic_get(&app->memory_used));

    /* Return pointer past header */
    return (void *)(hdr + 1);
#else
    (void)exec_env;
    return akira_malloc_buffer(size);
#endif
}

/**
 * @brief Free memory allocated with akira_wasm_malloc
 *
 * Updates quota tracking and frees the underlying memory.
 * Safe to call with NULL pointer.
 *
 * @param exec_env  WAMR execution environment
 * @param ptr       Pointer to memory (from akira_wasm_malloc)
 */
void akira_wasm_free(wasm_exec_env_t exec_env, void *ptr)
{
    if (!ptr) {
        return;
    }

#ifdef CONFIG_AKIRA_WASM_RUNTIME
    /* Retrieve header */
    akira_alloc_header_t *hdr = ((akira_alloc_header_t *)ptr) - 1;

    /* Validate magic */
    if (hdr->magic != AKIRA_ALLOC_MAGIC) {
        LOG_ERR("wasm_free: invalid pointer or corrupted header at %p", ptr);
        return;  /* Don't crash - graceful failure */
    }

    int slot = hdr->app_slot;
    size_t total_size = hdr->size + sizeof(akira_alloc_header_t);

    /* Update quota tracking */
    if (slot >= 0 && slot < AKIRA_MAX_WASM_INSTANCES && g_apps[slot].used) {
        if (atomic_get(&g_apps[slot].memory_used) >= (atomic_val_t)total_size) {
            atomic_sub(&g_apps[slot].memory_used, (atomic_val_t)total_size);
        } else {
            LOG_WRN("wasm_free: memory accounting underflow for app %s", g_apps[slot].name);
            atomic_set(&g_apps[slot].memory_used, 0);
        }
        LOG_DBG("wasm_free: app %s freed %u bytes (remaining: %ld)",
                g_apps[slot].name, hdr->size, atomic_get(&g_apps[slot].memory_used));
    }

    /* Clear magic to detect double-free */
    hdr->magic = 0;

    /* Free the underlying buffer */
    akira_free_buffer(hdr);
#else
    (void)exec_env;
    akira_free_buffer(ptr);
#endif
}

/* WASM Native export api */

/* Bytes reserved in front of each mem_alloc block to record its size for
 * quota accounting. 8 bytes keeps the app-visible pointer 8-byte aligned. */
#define AKIRA_WASM_ALLOC_HDR 8u

uint32_t akira_native_mem_alloc(wasm_exec_env_t exec_env, uint32_t size)
{
#ifdef CONFIG_AKIRA_WASM_RUNTIME
    /* Capability gate: only apps that declared "memory" in their manifest
     * are allowed to call mem_alloc. Quota is still enforced on top of this. */
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MEMORY, 0);

    if (size == 0 || size > (16 * 1024 * 1024)) {  /* Sanity limit: 16MB */
        return 0;
    }

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return 0;
    }

    int slot = get_slot_for_module_inst(module_inst);
    if (slot < 0) {
        return 0;
    }
    akira_managed_app_t *app = &g_apps[slot];

    /* Allocate size + header so free() can recover the block size. */
    uint32_t total = size + AKIRA_WASM_ALLOC_HDR;

    /* Enforce the per-app quota before allocating (quota == 0 => unlimited). */
    uint32_t quota = app->memory_quota;
    if (quota > 0 && (uint32_t)atomic_get(&app->memory_used) + total > quota) {
        LOG_WRN("mem_alloc: quota exceeded for app %s (used=%ld, req=%u, quota=%u)",
                app->name, atomic_get(&app->memory_used), total, quota);
        return 0;
    }

    uint32_t base = (uint32_t)wasm_runtime_module_malloc(module_inst, total, NULL);
    if (base == 0) {
        LOG_WRN("mem_alloc: WAMR module malloc failed for %u bytes", total);
        return 0;
    }

    /* Record the block size in the header, validating the range first. */
    if (!wasm_runtime_validate_app_addr(module_inst, base, AKIRA_WASM_ALLOC_HDR)) {
        wasm_runtime_module_free(module_inst, base);
        return 0;
    }
    uint32_t *hdr = (uint32_t *)wasm_runtime_addr_app_to_native(module_inst, base);
    if (!hdr) {
        wasm_runtime_module_free(module_inst, base);
        return 0;
    }
    *hdr = total;

    atomic_add(&app->memory_used, (atomic_val_t)total);
    LOG_DBG("mem_alloc: app %s allocated %u bytes (used: %ld)",
            app->name, size, atomic_get(&app->memory_used));

    /* Hand the app the region just past the header. */
    return base + AKIRA_WASM_ALLOC_HDR;
#else
    (void)exec_env; (void)size;
    return 0;
#endif
}

void akira_native_mem_free(wasm_exec_env_t exec_env, uint32_t ptr)
{
#ifdef CONFIG_AKIRA_WASM_RUNTIME
    /* Capability gate: only apps that declared "memory" can call mem_free.
     * This prevents a rogue app from freeing pointers obtained by other means. */
    if (!akira_security_check_exec(exec_env, AKIRA_CAP_MEMORY)) {
        LOG_WRN("mem_free: capability denied");
        return;
    }

    if (ptr == 0 || ptr < AKIRA_WASM_ALLOC_HDR) {
        return;
    }

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return;
    }

    /* Recover the header written by mem_alloc() to learn the block size,
     * validating the range so a forged pointer cannot cause an OOB read. */
    uint32_t base = ptr - AKIRA_WASM_ALLOC_HDR;
    if (!wasm_runtime_validate_app_addr(module_inst, base, AKIRA_WASM_ALLOC_HDR)) {
        LOG_WRN("mem_free: invalid pointer 0x%08x", ptr);
        return;
    }
    uint32_t total = *(uint32_t *)wasm_runtime_addr_app_to_native(module_inst, base);

    /* Return the block's bytes to the app's quota. */
    int slot = get_slot_for_module_inst(module_inst);
    if (slot >= 0 && slot < AKIRA_MAX_WASM_INSTANCES && g_apps[slot].used) {
        atomic_val_t used = atomic_get(&g_apps[slot].memory_used);
        if (used >= (atomic_val_t)total) {
            atomic_sub(&g_apps[slot].memory_used, (atomic_val_t)total);
        } else {
            LOG_WRN("mem_free: quota accounting underflow for app %s", g_apps[slot].name);
            atomic_set(&g_apps[slot].memory_used, 0);
        }
        LOG_DBG("mem_free: app %s freed %u bytes (remaining: %ld)",
                g_apps[slot].name, total, atomic_get(&g_apps[slot].memory_used));
    }

    wasm_runtime_module_free(module_inst, base);
#else
    (void)exec_env; (void)ptr;
#endif
}
#endif