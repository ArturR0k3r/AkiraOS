/**
 * @file memory.c
 * @brief AkiraOS Memory Management Implementation
 */

#include "memory.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_memory, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Internal Structures                                                       */
/*===========================================================================*/

struct akira_pool {
    const char *name;
    akira_pool_type_t type;
    size_t total_size;
    size_t block_size;
    void *buffer;
    bool owns_buffer;
    uint32_t flags;
    
    /* Statistics */
    size_t used_bytes;
    size_t peak_usage;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t alloc_failures;
    
    /* Synchronization */
    struct k_mutex mutex;
    
    /* Type-specific data */
    union {
        struct {
            struct k_mem_slab slab;
        } fixed;
        struct {
            struct k_heap heap;
        } variable;
    };
};

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct {
    bool initialized;
    akira_pool_t pools[AKIRA_MAX_MEMORY_POOLS];
    int pool_count;
    struct k_mutex mutex;
    
    /* Global stats */
    akira_mem_stats_t global_stats;
} mem_mgr;

/*===========================================================================*/
/* Memory Pool Implementation                                                */
/*===========================================================================*/

int akira_memory_init(void)
{
    if (mem_mgr.initialized) {
        return 0;
    }
    
    LOG_INF("Initializing memory subsystem");
    
    k_mutex_init(&mem_mgr.mutex);
    memset(&mem_mgr.pools, 0, sizeof(mem_mgr.pools));
    memset(&mem_mgr.global_stats, 0, sizeof(mem_mgr.global_stats));
    mem_mgr.pool_count = 0;
    
    mem_mgr.initialized = true;
    
    LOG_INF("Memory subsystem initialized");
    
    return 0;
}

akira_pool_t *akira_pool_create(const akira_pool_config_t *config)
{
    if (!mem_mgr.initialized || !config) {
        return NULL;
    }
    
    k_mutex_lock(&mem_mgr.mutex, K_FOREVER);
    
    /* Find free pool slot */
    akira_pool_t *pool = NULL;
    for (int i = 0; i < AKIRA_MAX_MEMORY_POOLS; i++) {
        if (mem_mgr.pools[i].name == NULL) {
            pool = &mem_mgr.pools[i];
            break;
        }
    }
    
    if (!pool) {
        k_mutex_unlock(&mem_mgr.mutex);
        LOG_ERR("No free pool slots");
        return NULL;
    }
    
    /* Initialize pool */
    memset(pool, 0, sizeof(*pool));
    pool->name = config->name ? config->name : "unnamed";
    pool->type = config->type;
    pool->total_size = config->total_size;
    pool->block_size = config->block_size > 0 ? 
                       config->block_size : AKIRA_POOL_DEFAULT_BLOCK;
    pool->flags = config->flags;
    
    k_mutex_init(&pool->mutex);
    
    /* Allocate buffer if not provided */
    if (config->buffer) {
        pool->buffer = config->buffer;
        pool->owns_buffer = false;
    } else {
        pool->buffer = k_malloc(config->total_size);
        if (!pool->buffer) {
            memset(pool, 0, sizeof(*pool));
            k_mutex_unlock(&mem_mgr.mutex);
            LOG_ERR("Failed to allocate pool buffer");
            return NULL;
        }
        pool->owns_buffer = true;
    }
    
    /* Initialize type-specific allocator */
    int ret = 0;
    switch (config->type) {
    case AKIRA_POOL_FIXED:
        ret = k_mem_slab_init(&pool->fixed.slab, pool->buffer,
                              pool->block_size,
                              pool->total_size / pool->block_size);
        break;
        
    case AKIRA_POOL_VARIABLE:
        k_heap_init(&pool->variable.heap, pool->buffer, pool->total_size);
        break;
        
    case AKIRA_POOL_SLAB:
        /* Same as fixed for now */
        ret = k_mem_slab_init(&pool->fixed.slab, pool->buffer,
                              pool->block_size,
                              pool->total_size / pool->block_size);
        break;
    }
    
    if (ret < 0) {
        if (pool->owns_buffer) {
            k_free(pool->buffer);
        }
        memset(pool, 0, sizeof(*pool));
        k_mutex_unlock(&mem_mgr.mutex);
        LOG_ERR("Failed to initialize pool allocator");
        return NULL;
    }
    
    mem_mgr.pool_count++;
    mem_mgr.global_stats.total_bytes += config->total_size;
    mem_mgr.global_stats.free_bytes += config->total_size;
    
    k_mutex_unlock(&mem_mgr.mutex);
    
    LOG_INF("Created pool '%s' (type=%d, size=%zu)", 
            pool->name, pool->type, pool->total_size);
    
    return pool;
}

void akira_pool_destroy(akira_pool_t *pool)
{
    if (!pool || !pool->name) {
        return;
    }
    
    k_mutex_lock(&mem_mgr.mutex, K_FOREVER);
    
    LOG_INF("Destroying pool '%s'", pool->name);
    
    mem_mgr.global_stats.total_bytes -= pool->total_size;
    mem_mgr.global_stats.free_bytes -= (pool->total_size - pool->used_bytes);
    mem_mgr.global_stats.used_bytes -= pool->used_bytes;
    
    if (pool->owns_buffer && pool->buffer) {
        k_free(pool->buffer);
    }
    
    memset(pool, 0, sizeof(*pool));
    mem_mgr.pool_count--;
    
    k_mutex_unlock(&mem_mgr.mutex);
}

void *akira_pool_alloc(akira_pool_t *pool, size_t size)
{
    if (!pool) {
        return NULL;
    }
    
    void *ptr = NULL;
    
    k_mutex_lock(&pool->mutex, K_FOREVER);
    
    switch (pool->type) {
    case AKIRA_POOL_FIXED:
    case AKIRA_POOL_SLAB:
        if (size > pool->block_size) {
            pool->alloc_failures++;
            k_mutex_unlock(&pool->mutex);
            return NULL;
        }
        if (k_mem_slab_alloc(&pool->fixed.slab, &ptr, K_NO_WAIT) != 0) {
            pool->alloc_failures++;
            ptr = NULL;
        } else {
            pool->used_bytes += pool->block_size;
        }
        break;
        
    case AKIRA_POOL_VARIABLE:
        ptr = k_heap_alloc(&pool->variable.heap, size, K_NO_WAIT);
        if (ptr) {
            pool->used_bytes += size;
        } else {
            pool->alloc_failures++;
        }
        break;
    }
    
    if (ptr) {
        pool->alloc_count++;
        if (pool->used_bytes > pool->peak_usage) {
            pool->peak_usage = pool->used_bytes;
        }
    }
    
    k_mutex_unlock(&pool->mutex);
    
    return ptr;
}

void *akira_pool_calloc(akira_pool_t *pool, size_t count, size_t size)
{
    size_t total = count * size;
    void *ptr = akira_pool_alloc(pool, total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void akira_pool_free(akira_pool_t *pool, void *ptr)
{
    if (!pool || !ptr) {
        return;
    }
    
    k_mutex_lock(&pool->mutex, K_FOREVER);
    
    switch (pool->type) {
    case AKIRA_POOL_FIXED:
    case AKIRA_POOL_SLAB:
        k_mem_slab_free(&pool->fixed.slab, ptr);
        pool->used_bytes -= pool->block_size;
        break;
        
    case AKIRA_POOL_VARIABLE:
        k_heap_free(&pool->variable.heap, ptr);
        /* Note: Can't track exact size freed for variable pools */
        break;
    }
    
    pool->free_count++;
    
    k_mutex_unlock(&pool->mutex);
}

int akira_pool_stats(akira_pool_t *pool, akira_mem_stats_t *stats)
{
    if (!pool || !stats) {
        return -1;
    }
    
    k_mutex_lock(&pool->mutex, K_FOREVER);
    
    stats->total_bytes = pool->total_size;
    stats->used_bytes = pool->used_bytes;
    stats->free_bytes = pool->total_size - pool->used_bytes;
    stats->peak_usage = pool->peak_usage;
    stats->alloc_count = pool->alloc_count;
    stats->free_count = pool->free_count;
    stats->alloc_failures = pool->alloc_failures;
    
    k_mutex_unlock(&pool->mutex);
    
    return 0;
}

int akira_pool_free_count(akira_pool_t *pool)
{
    if (!pool || pool->type == AKIRA_POOL_VARIABLE) {
        return -1;
    }
    
    return k_mem_slab_num_free_get(&pool->fixed.slab);
}

/*===========================================================================*/
/* System Heap Implementation                                                */
/*===========================================================================*/

void *akira_malloc(size_t size)
{
    void *ptr = k_malloc(size);
    if (ptr && mem_mgr.initialized) {
        k_mutex_lock(&mem_mgr.mutex, K_FOREVER);
        mem_mgr.global_stats.alloc_count++;
        mem_mgr.global_stats.used_bytes += size;
        if (mem_mgr.global_stats.used_bytes > mem_mgr.global_stats.peak_usage) {
            mem_mgr.global_stats.peak_usage = mem_mgr.global_stats.used_bytes;
        }
        k_mutex_unlock(&mem_mgr.mutex);
    }
    return ptr;
}

void *akira_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *ptr = akira_malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *akira_realloc(void *ptr, size_t size)
{
    /* Zephyr doesn't have realloc, so we implement a basic version */
    if (!ptr) {
        return akira_malloc(size);
    }
    
    if (size == 0) {
        akira_free(ptr);
        return NULL;
    }
    
    /* Simple implementation: allocate new, copy, free old */
    void *new_ptr = akira_malloc(size);
    if (new_ptr) {
        /* Note: We can't know the original size, caller must handle this */
        memcpy(new_ptr, ptr, size);
        akira_free(ptr);
    }
    return new_ptr;
}

void akira_free(void *ptr)
{
    if (!ptr) {
        return;
    }
    
    if (mem_mgr.initialized) {
        k_mutex_lock(&mem_mgr.mutex, K_FOREVER);
        mem_mgr.global_stats.free_count++;
        k_mutex_unlock(&mem_mgr.mutex);
    }
    
    k_free(ptr);
}

void *akira_aligned_alloc(size_t alignment, size_t size)
{
    return k_aligned_alloc(alignment, size);
}

void akira_aligned_free(void *ptr)
{
    k_free(ptr);
}

/*===========================================================================*/
/* Memory Region Implementation (Stubs)                                      */
/*===========================================================================*/

akira_mem_region_t *akira_region_create(size_t size, uint32_t flags)
{
    akira_mem_region_t *region = akira_malloc(sizeof(akira_mem_region_t));
    if (!region) {
        return NULL;
    }
    
    region->base = akira_malloc(size);
    if (!region->base) {
        akira_free(region);
        return NULL;
    }
    
    region->size = size;
    region->flags = flags;
    region->owner = 0;
    
    return region;
}

void akira_region_destroy(akira_mem_region_t *region)
{
    if (!region) return;
    
    if (region->base) {
        akira_free(region->base);
    }
    akira_free(region);
}

int akira_region_protect(akira_mem_region_t *region, uint32_t flags)
{
    if (!region) return -1;
    
    /* Memory protection requires MPU support */
    region->flags = flags;
    
    /* TODO: Configure MPU if available */
    
    return 0;
}

int akira_region_map(akira_mem_region_t *region, akira_pid_t pid)
{
    if (!region) return -1;
    
    region->owner = pid;
    
    /* TODO: Update process memory map */
    
    return 0;
}

int akira_region_unmap(akira_mem_region_t *region, akira_pid_t pid)
{
    if (!region) return -1;
    
    if (region->owner == pid) {
        region->owner = 0;
    }
    
    return 0;
}

/*===========================================================================*/
/* Statistics & Debugging                                                    */
/*===========================================================================*/

int akira_memory_stats(akira_mem_stats_t *stats)
{
    if (!stats) return -1;
    
    k_mutex_lock(&mem_mgr.mutex, K_FOREVER);
    *stats = mem_mgr.global_stats;
    k_mutex_unlock(&mem_mgr.mutex);
    
    return 0;
}

size_t akira_memory_usage(akira_pid_t pid)
{
    /* TODO: Track per-process memory usage */
    (void)pid;
    return 0;
}

int akira_memory_check_leaks(void)
{
    int leaks = 0;
    
    k_mutex_lock(&mem_mgr.mutex, K_FOREVER);
    leaks = mem_mgr.global_stats.alloc_count - mem_mgr.global_stats.free_count;
    k_mutex_unlock(&mem_mgr.mutex);
    
    if (leaks > 0) {
        LOG_WRN("Detected %d potential memory leaks", leaks);
    }
    
    return leaks;
}

void akira_memory_dump(void)
{
    LOG_INF("=== Memory State ===");
    LOG_INF("Total: %zu bytes", mem_mgr.global_stats.total_bytes);
    LOG_INF("Used: %zu bytes", mem_mgr.global_stats.used_bytes);
    LOG_INF("Free: %zu bytes", mem_mgr.global_stats.free_bytes);
    LOG_INF("Peak: %zu bytes", mem_mgr.global_stats.peak_usage);
    LOG_INF("Allocs: %u, Frees: %u", 
            mem_mgr.global_stats.alloc_count,
            mem_mgr.global_stats.free_count);
    LOG_INF("Pools: %d", mem_mgr.pool_count);
    
    for (int i = 0; i < AKIRA_MAX_MEMORY_POOLS; i++) {
        if (mem_mgr.pools[i].name) {
            LOG_INF("  Pool '%s': %zu/%zu bytes used",
                    mem_mgr.pools[i].name,
                    mem_mgr.pools[i].used_bytes,
                    mem_mgr.pools[i].total_size);
        }
    }
}
