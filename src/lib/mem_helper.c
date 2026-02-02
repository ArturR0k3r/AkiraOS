/**
 * @file mem_helper.c
 * @brief Memory allocation helper with PSRAM/SRAM fallback
 *
 * Implements unified memory allocation that prefers PSRAM when available.
 * This reduces peak SRAM usage during WASM loading by offloading buffers
 * to external memory.
 */

#include "mem_helper.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(mem_helper, CONFIG_LOG_DEFAULT_LEVEL);

/* PSRAM detection via shared multi-heap */
#ifdef CONFIG_AKIRA_PSRAM
#include <zephyr/multi_heap/shared_multi_heap.h>

#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
#include <soc/soc_memory_layout.h>
#define HAS_ESP_PTR_CHECK 1
#else
#define HAS_ESP_PTR_CHECK 0
#endif

#endif /* CONFIG_AKIRA_PSRAM */

/**
 * @brief Check if pointer is in external PSRAM
 */
static bool is_psram_ptr(void *ptr)
{
#ifdef CONFIG_AKIRA_PSRAM
#if HAS_ESP_PTR_CHECK
    return esp_ptr_external_ram(ptr);
#else
    /* For non-ESP platforms, assume any allocation from SMH_REG_ATTR_EXTERNAL
     * is external. We track this via allocation source. */
    (void)ptr;
    return false;
#endif
#else
    (void)ptr;
    return false;
#endif
}

mem_source_t akira_get_mem_source(void *ptr)
{
    if (!ptr) {
        return MEM_SOURCE_UNKNOWN;
    }
    
#ifdef CONFIG_AKIRA_PSRAM
    if (is_psram_ptr(ptr)) {
        return MEM_SOURCE_PSRAM;
    }
#endif
    return MEM_SOURCE_SRAM;
}

void *akira_malloc_buffer(size_t size)
{
    return akira_malloc_buffer_ex(size, NULL);
}

void *akira_malloc_buffer_ex(size_t size, mem_source_t *source)
{
    void *ptr = NULL;
    mem_source_t src = MEM_SOURCE_UNKNOWN;

    if (size == 0) {
        if (source) *source = MEM_SOURCE_UNKNOWN;
        return NULL;
    }

#ifdef CONFIG_AKIRA_PSRAM
    /* Try PSRAM first via shared multi-heap */
    ptr = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, size);
    if (ptr) {
#if HAS_ESP_PTR_CHECK
        /* Verify it's actually in external RAM on ESP platforms */
        if (esp_ptr_external_ram(ptr)) {
            src = MEM_SOURCE_PSRAM;
            LOG_DBG("Allocated %zu bytes from PSRAM at %p", size, ptr);
        } else {
            /* Allocation succeeded but not in PSRAM - free and try SRAM */
            LOG_WRN("PSRAM alloc returned non-external ptr, falling back");
            shared_multi_heap_free(ptr);
            ptr = NULL;
        }
#else
        /* Assume external allocation is PSRAM on non-ESP */
        src = MEM_SOURCE_PSRAM;
        LOG_DBG("Allocated %zu bytes from external heap at %p", size, ptr);
#endif
    }
#endif

    /* Fallback to internal SRAM */
    if (!ptr) {
        ptr = k_malloc(size);
        if (ptr) {
            src = MEM_SOURCE_SRAM;
            LOG_DBG("Allocated %zu bytes from SRAM at %p", size, ptr);
        } else {
            LOG_ERR("Failed to allocate %zu bytes (PSRAM and SRAM)", size);
        }
    }

    if (source) {
        *source = src;
    }

    return ptr;
}

void akira_free_buffer(void *ptr)
{
    if (!ptr) {
        return;
    }

#ifdef CONFIG_AKIRA_PSRAM
    if (is_psram_ptr(ptr)) {
        shared_multi_heap_free(ptr);
        LOG_DBG("Freed PSRAM buffer at %p", ptr);
        return;
    }
#endif

    /* Must be SRAM allocation */
    k_free(ptr);
    LOG_DBG("Freed SRAM buffer at %p", ptr);
}

void *akira_malloc_aligned(size_t size, size_t align)
{
    if (size == 0 || align == 0 || (align & (align - 1)) != 0) {
        return NULL;  /* Invalid alignment (must be power of 2) */
    }

    size_t padded = size + align - 1 + sizeof(void *);
    void *raw = NULL;

#ifdef CONFIG_AKIRA_PSRAM
    /* Try PSRAM first */
    raw = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, padded);
    if (raw) {
#if HAS_ESP_PTR_CHECK
        if (!esp_ptr_external_ram(raw)) {
            shared_multi_heap_free(raw);
            raw = NULL;
        } else
#endif
        {
            uintptr_t addr = (uintptr_t)raw + sizeof(void *);
            addr = (addr + align - 1) & ~(align - 1);
            ((void **)addr)[-1] = raw;  /* Store original pointer */
            LOG_DBG("Allocated %zu aligned bytes from PSRAM", size);
            return (void *)addr;
        }
    }
#endif

    /* Fallback to SRAM with manual alignment */
    raw = k_malloc(padded);
    if (raw) {
        uintptr_t addr = (uintptr_t)raw + sizeof(void *);
        addr = (addr + align - 1) & ~(align - 1);
        ((void **)addr)[-1] = raw;  /* Store original pointer */
        LOG_DBG("Allocated %zu aligned bytes from SRAM", size);
        return (void *)addr;
    }

    LOG_ERR("Failed to allocate %zu aligned bytes", size);
    return NULL;
}

void akira_free_aligned(void *ptr)
{
    if (!ptr) {
        return;
    }

    /* Retrieve original pointer stored before aligned address */
    void *raw = ((void **)ptr)[-1];

#ifdef CONFIG_AKIRA_PSRAM
    if (is_psram_ptr(raw)) {
        shared_multi_heap_free(raw);
        return;
    }
#endif

    k_free(raw);
}
