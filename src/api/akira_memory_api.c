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

    /* Check quota before allocation (atomic-safe read) */
    uint32_t quota = app->memory_quota;
    if (quota > 0) {
        uint32_t current = app->memory_used;
        if (current + total_size > quota) {
            LOG_WRN("wasm_malloc: quota exceeded for app %s (used=%u, req=%zu, quota=%u)",
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

    /* Update quota tracking (simple increment - could use atomic for ISR safety) */
    app->memory_used += total_size;

    LOG_DBG("wasm_malloc: app %s allocated %zu bytes (total used: %u)",
            app->name, size, app->memory_used);

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
        if (g_apps[slot].memory_used >= total_size) {
            g_apps[slot].memory_used -= total_size;
        } else {
            LOG_WRN("wasm_free: memory accounting underflow for app %s", g_apps[slot].name);
            g_apps[slot].memory_used = 0;
        }
        LOG_DBG("wasm_free: app %s freed %u bytes (remaining: %u)",
                g_apps[slot].name, hdr->size, g_apps[slot].memory_used);
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

uint32_t akira_native_mem_alloc(wasm_exec_env_t exec_env, uint32_t size)
{
#ifdef CONFIG_AKIRA_WASM_RUNTIME
    if (size == 0 || size > (16 * 1024 * 1024)) {  /* Sanity limit: 16MB */
        return 0;
    }

    /* Allocate from quota-enforced pool */
    void *ptr = akira_wasm_malloc(exec_env, size);
    if (!ptr) {
        return 0;  /* Quota exceeded or allocation failed */
    }

    /* Convert native pointer to WASM address space */
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        akira_wasm_free(exec_env, ptr);
        return 0;
    }

    /* The allocated memory is outside WASM linear memory, so we need to
     * use module heap allocation for WASM-accessible memory.
     * For now, return 0 as this requires WAMR's module_malloc.
     *
     * TODO: Use wasm_runtime_module_malloc() for WASM-accessible allocation
     */
    akira_wasm_free(exec_env, ptr);

    /* Use WAMR's module malloc for WASM-accessible memory with quota check */
    int slot = get_slot_for_module_inst(module_inst);
    if (slot < 0) {
        return 0;
    }

    akira_managed_app_t *app = &g_apps[slot];

    /* Check quota before WAMR allocation */
    uint32_t quota = app->memory_quota;
    if (quota > 0 && app->memory_used + size > quota) {
        LOG_WRN("mem_alloc: quota exceeded for app %s", app->name);
        return 0;
    }

    uint32_t wasm_ptr = (uint32_t)wasm_runtime_module_malloc(module_inst, size, NULL);
    if (wasm_ptr == 0) {
        LOG_WRN("mem_alloc: WAMR module malloc failed");
        return 0;
    }

    /* Track allocation in quota */
    app->memory_used += size;
    LOG_DBG("mem_alloc: app %s allocated %u bytes (used: %u)", app->name, size, app->memory_used);

    return wasm_ptr;
#else
    (void)exec_env; (void)size;
    return 0;
#endif
}

void akira_native_mem_free(wasm_exec_env_t exec_env, uint32_t ptr)
{
#ifdef CONFIG_AKIRA_WASM_RUNTIME
    if (ptr == 0) {
        return;
    }

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return;
    }

    int slot = get_slot_for_module_inst(module_inst);
    if (slot >= 0) {
        /* Note: We can't easily track size for WAMR module_free
         * For accurate quota tracking, we'd need to store allocation sizes
         * or use WAMR's internal tracking. For now, we just free.
         */
        LOG_DBG("mem_free: app %s freeing ptr 0x%08x", g_apps[slot].name, ptr);
    }

    wasm_runtime_module_free(module_inst, ptr);
#else
    (void)exec_env; (void)ptr;
#endif
}
#endif