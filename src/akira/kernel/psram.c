/**
 * @file psram.c
 * @brief AkiraOS PSRAM Management Implementation
 *
 * Provides external PSRAM allocation using Zephyr's shared_multi_heap.
 * On ESP32-S3 N16R8, this provides access to 8MB of external OPI PSRAM.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "psram.h"
#include "memory.h"

LOG_MODULE_REGISTER(akira_psram, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Platform-specific includes                                                */
/*===========================================================================*/

#if defined(CONFIG_ESP_SPIRAM)
#include <zephyr/multi_heap/shared_multi_heap.h>

/* ESP32-S3 PSRAM address range (memory-mapped) */
#define ESP32S3_PSRAM_START 0x3C000000
#define ESP32S3_PSRAM_END 0x3DFFFFFF
#endif

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    bool available;
    size_t total_size;
    struct k_mutex mutex;

    /* Statistics */
    size_t used_bytes;
    size_t peak_usage;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t alloc_failures;
} psram_state = {
    .initialized = false,
    .available = false,
    .total_size = 0,
};

/*===========================================================================*/
/* Internal Functions                                                        */
/*===========================================================================*/

static void psram_init_once(void)
{
    if (psram_state.initialized)
    {
        return;
    }

    k_mutex_init(&psram_state.mutex);

#if defined(CONFIG_ESP_SPIRAM)
    psram_state.available = true;
    psram_state.total_size = CONFIG_ESP_SPIRAM_HEAP_SIZE;
    LOG_INF("PSRAM initialized: %zu bytes available", psram_state.total_size);
#else
    psram_state.available = false;
    psram_state.total_size = 0;
    LOG_WRN("PSRAM not available on this platform");
#endif

    psram_state.initialized = true;
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

bool akira_psram_available(void)
{
    psram_init_once();
    return psram_state.available;
}

size_t akira_psram_get_size(void)
{
    psram_init_once();
    return psram_state.total_size;
}

size_t akira_psram_get_free(void)
{
    psram_init_once();

    if (!psram_state.available)
    {
        return 0;
    }

    k_mutex_lock(&psram_state.mutex, K_FOREVER);
    size_t free = psram_state.total_size - psram_state.used_bytes;
    k_mutex_unlock(&psram_state.mutex);

    return free;
}

void *akira_psram_alloc(size_t size)
{
    psram_init_once();

    if (!psram_state.available || size == 0)
    {
        return NULL;
    }

#if defined(CONFIG_ESP_SPIRAM)
    void *ptr = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, size);

    if (ptr)
    {
        k_mutex_lock(&psram_state.mutex, K_FOREVER);
        psram_state.used_bytes += size;
        psram_state.alloc_count++;
        if (psram_state.used_bytes > psram_state.peak_usage)
        {
            psram_state.peak_usage = psram_state.used_bytes;
        }
        k_mutex_unlock(&psram_state.mutex);

        LOG_DBG("PSRAM alloc: %zu bytes at %p", size, ptr);
    }
    else
    {
        k_mutex_lock(&psram_state.mutex, K_FOREVER);
        psram_state.alloc_failures++;
        k_mutex_unlock(&psram_state.mutex);

        LOG_WRN("PSRAM alloc failed: %zu bytes", size);
    }

    return ptr;
#else
    /* Fallback to regular malloc if no PSRAM */
    return akira_malloc(size);
#endif
}

void *akira_psram_aligned_alloc(size_t alignment, size_t size)
{
    psram_init_once();

    if (!psram_state.available || size == 0)
    {
        return NULL;
    }

#if defined(CONFIG_ESP_SPIRAM)
    void *ptr = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, alignment, size);

    if (ptr)
    {
        k_mutex_lock(&psram_state.mutex, K_FOREVER);
        psram_state.used_bytes += size;
        psram_state.alloc_count++;
        if (psram_state.used_bytes > psram_state.peak_usage)
        {
            psram_state.peak_usage = psram_state.used_bytes;
        }
        k_mutex_unlock(&psram_state.mutex);

        LOG_DBG("PSRAM aligned alloc: %zu bytes (align=%zu) at %p", size, alignment, ptr);
    }
    else
    {
        k_mutex_lock(&psram_state.mutex, K_FOREVER);
        psram_state.alloc_failures++;
        k_mutex_unlock(&psram_state.mutex);

        LOG_WRN("PSRAM aligned alloc failed: %zu bytes", size);
    }

    return ptr;
#else
    return akira_aligned_alloc(alignment, size);
#endif
}

void *akira_psram_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *ptr = akira_psram_alloc(total);

    if (ptr)
    {
        memset(ptr, 0, total);
    }

    return ptr;
}

void akira_psram_free(void *ptr)
{
    if (!ptr)
    {
        return;
    }

#if defined(CONFIG_ESP_SPIRAM)
    if (akira_psram_ptr_is_psram(ptr))
    {
        shared_multi_heap_free(ptr);

        k_mutex_lock(&psram_state.mutex, K_FOREVER);
        psram_state.free_count++;
        /* Note: We can't track exact freed size with shared_multi_heap */
        k_mutex_unlock(&psram_state.mutex);

        LOG_DBG("PSRAM free: %p", ptr);
    }
    else
    {
        /* Not PSRAM, use regular free */
        akira_free(ptr);
    }
#else
    akira_free(ptr);
#endif
}

bool akira_psram_ptr_is_psram(const void *ptr)
{
#if defined(CONFIG_ESP_SPIRAM) && defined(CONFIG_SOC_SERIES_ESP32S3)
    uintptr_t addr = (uintptr_t)ptr;
    return (addr >= ESP32S3_PSRAM_START && addr <= ESP32S3_PSRAM_END);
#else
    (void)ptr;
    return false;
#endif
}

int akira_psram_get_stats(akira_psram_stats_t *stats)
{
    if (!stats)
    {
        return -EINVAL;
    }

    psram_init_once();

    k_mutex_lock(&psram_state.mutex, K_FOREVER);

    stats->total_bytes = psram_state.total_size;
    stats->used_bytes = psram_state.used_bytes;
    stats->free_bytes = psram_state.total_size - psram_state.used_bytes;
    stats->peak_usage = psram_state.peak_usage;
    stats->alloc_count = psram_state.alloc_count;
    stats->free_count = psram_state.free_count;
    stats->alloc_failures = psram_state.alloc_failures;

    k_mutex_unlock(&psram_state.mutex);

    return 0;
}

void akira_psram_dump_stats(void)
{
    akira_psram_stats_t stats;

    if (akira_psram_get_stats(&stats) < 0)
    {
        LOG_ERR("Failed to get PSRAM stats");
        return;
    }

    LOG_INF("=== PSRAM Status ===");
    LOG_INF("Available: %s", psram_state.available ? "Yes" : "No");

    if (psram_state.available)
    {
        LOG_INF("Total: %zu bytes (%.2f MB)",
                stats.total_bytes,
                (double)stats.total_bytes / (1024.0 * 1024.0));
        LOG_INF("Used: %zu bytes (%.1f%%)",
                stats.used_bytes,
                (double)stats.used_bytes * 100.0 / stats.total_bytes);
        LOG_INF("Free: %zu bytes (%.2f MB)",
                stats.free_bytes,
                (double)stats.free_bytes / (1024.0 * 1024.0));
        LOG_INF("Peak: %zu bytes", stats.peak_usage);
        LOG_INF("Allocs: %u, Frees: %u, Failures: %u",
                stats.alloc_count, stats.free_count, stats.alloc_failures);
    }
}

/*===========================================================================*/
/* PSRAM Pool API                                                            */
/*===========================================================================*/

void *akira_psram_pool_create(const char *name, size_t size)
{
    if (!akira_psram_available())
    {
        LOG_ERR("Cannot create PSRAM pool: PSRAM not available");
        return NULL;
    }

    /* Allocate pool buffer from PSRAM */
    void *buffer = akira_psram_alloc(size);
    if (!buffer)
    {
        LOG_ERR("Failed to allocate PSRAM pool '%s' (%zu bytes)",
                name ? name : "unnamed", size);
        return NULL;
    }

    /* Create Akira pool using this buffer */
    akira_pool_config_t config = {
        .name = name,
        .type = AKIRA_POOL_VARIABLE,
        .total_size = size,
        .buffer = buffer,
        .flags = 0,
    };

    akira_pool_t *pool = akira_pool_create(&config);
    if (!pool)
    {
        akira_psram_free(buffer);
        LOG_ERR("Failed to create pool structure for PSRAM pool '%s'",
                name ? name : "unnamed");
        return NULL;
    }

    LOG_INF("Created PSRAM pool '%s': %zu bytes at %p",
            name ? name : "unnamed", size, buffer);

    return pool;
}

void akira_psram_pool_destroy(void *pool)
{
    if (!pool)
    {
        return;
    }

    /* The pool will free its buffer when destroyed */
    akira_pool_destroy((akira_pool_t *)pool);
}
