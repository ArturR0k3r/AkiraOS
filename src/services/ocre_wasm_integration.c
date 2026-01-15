/**
 * @file ocre_wasm_integration.c
 * @brief OCRE/WAMR Integration Functions
 *
 * Provides implementations for OCRE runtime functions needed by AkiraOS.
 * These wrap WAMR/OCRE functionality for AkiraOS use.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

/* WAMR headers */
#include "wasm_export.h"

LOG_MODULE_REGISTER(ocre_wasm_integration, CONFIG_AKIRA_LOG_LEVEL);

/* ===== Atomic Operations ===== */
/* GCC builtin atomics for 64-bit operations used by WAMR */

uint64_t __atomic_load_8(uint64_t *ptr, int memorder)
{
    (void)memorder;
    uint64_t value;
    __atomic_load(ptr, &value, __ATOMIC_SEQ_CST);
    return value;
}

void __atomic_store_8(uint64_t *ptr, uint64_t value, int memorder)
{
    (void)memorder;
    __atomic_store(ptr, &value, __ATOMIC_SEQ_CST);
}

/* ===== WAMR Memory Allocation Hooks ===== */
/* These are weak symbols that WAMR uses for memory allocation if defined */

void *user_malloc(size_t size)
{
    return k_malloc(size);
}

void user_free(void *ptr)
{
    k_free(ptr);
}

void *user_realloc(void *ptr, size_t size)
{
    /* Zephyr doesn't have krealloc, implement manually */
    if (ptr == NULL)
        return k_malloc(size);
    
    void *new_ptr = k_malloc(size);
    if (new_ptr == NULL)
        return NULL;
    
    /* We don't know the old size, so we can't copy. This is a limitation. */
    /* In practice, WAMR shouldn't be calling realloc much during embedded use */
    k_free(ptr);
    return new_ptr;
}

/* ===== Native Module Registration ===== */

/**
 * Registry for native modules
 * This allows multiple modules to be registered and used by WASM code
 */
typedef struct
{
    const char *module_name;
    NativeSymbol *symbols;
    uint32_t symbol_count;
} NativeModuleEntry;

#define MAX_NATIVE_MODULES 8
static NativeModuleEntry g_native_modules[MAX_NATIVE_MODULES] = {0};
static uint32_t g_module_count = 0;
static K_MUTEX_DEFINE(g_module_lock);

int ocre_register_native_module(const char *module_name, NativeSymbol *symbols, int count)
{
    if (!module_name || !symbols || count <= 0)
        return -EINVAL;

    k_mutex_lock(&g_module_lock, K_FOREVER);

    /* Check if module already registered */
    for (uint32_t i = 0; i < g_module_count; i++)
    {
        if (strcmp(g_native_modules[i].module_name, module_name) == 0)
        {
            LOG_WRN("Native module '%s' already registered", module_name);
            k_mutex_unlock(&g_module_lock);
            return -EEXIST;
        }
    }

    /* Add new module */
    if (g_module_count >= MAX_NATIVE_MODULES)
    {
        LOG_ERR("Max native modules (%d) reached", MAX_NATIVE_MODULES);
        k_mutex_unlock(&g_module_lock);
        return -ENOMEM;
    }

    g_native_modules[g_module_count].module_name = module_name;
    g_native_modules[g_module_count].symbols = symbols;
    g_native_modules[g_module_count].symbol_count = (uint32_t)count;
    g_module_count++;

    LOG_INF("Registered native module '%s' with %d symbols", module_name, count);

    k_mutex_unlock(&g_module_lock);
    return 0;
}

/**
 * Get registered native module by name
 * Used internally by WAMR to resolve native calls
 */
NativeSymbol *ocre_get_native_module(const char *module_name, uint32_t *count)
{
    if (!module_name || !count)
        return NULL;

    k_mutex_lock(&g_module_lock, K_FOREVER);

    for (uint32_t i = 0; i < g_module_count; i++)
    {
        if (strcmp(g_native_modules[i].module_name, module_name) == 0)
        {
            *count = g_native_modules[i].symbol_count;
            NativeSymbol *symbols = g_native_modules[i].symbols;
            k_mutex_unlock(&g_module_lock);
            return symbols;
        }
    }

    k_mutex_unlock(&g_module_lock);
    return NULL;
}

/* ===== File Management Stubs ===== */

/**
 * @brief Load a WASM file from filesystem
 *
 * This is a stub implementation. In a full OCRE setup, this would
 * load a WASM binary from the filesystem.
 */
int ocre_load_file(const char *path, void **buffer, size_t *size)
{
    if (!path || !buffer || !size)
        return -EINVAL;

    LOG_WRN("ocre_load_file stub: '%s'", path);
    
    /* TODO: Implement actual file loading from Zephyr filesystem */
    return -ENOSYS;
}

/**
 * @brief Unload a WASM file
 *
 * Frees the memory allocated by ocre_load_file
 */
int ocre_unload_file(void *buffer)
{
    if (!buffer)
        return -EINVAL;

    LOG_DBG("Unloading WASM file buffer");
    
    /* Free the buffer that was allocated by ocre_load_file */
    k_free(buffer);
    return 0;
}
