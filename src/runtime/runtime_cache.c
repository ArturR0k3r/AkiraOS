/**
 * @file runtime_cache.c
 * @brief WASM Module Cache & Instance Pool Implementation
 *
 * - LRU eviction for module cache
 * - Pointer-hash based instance→slot mapping
 * - Lock-free fast path where possible
 */

#include "runtime_cache.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(runtime_cache, CONFIG_AKIRA_LOG_LEVEL);

/* ===== Module Cache ===== */

static struct {
    module_cache_entry_t entries[CONFIG_AKIRA_MODULE_CACHE_SIZE];
    module_cache_stats_t stats;
    bool initialized;
} g_cache = {0};

static K_MUTEX_DEFINE(g_cache_mutex);

int module_cache_init(void)
{
    memset(&g_cache, 0, sizeof(g_cache));
    g_cache.initialized = true;
    LOG_INF("Module cache initialized (%d slots)", CONFIG_AKIRA_MODULE_CACHE_SIZE);
    return 0;
}

/**
 * @brief Find cache entry by hash (must hold mutex)
 */
static module_cache_entry_t *cache_find_locked(const uint8_t *hash)
{
    for (int i = 0; i < CONFIG_AKIRA_MODULE_CACHE_SIZE; i++) {
        if (g_cache.entries[i].used &&
            memcmp(g_cache.entries[i].hash, hash, 32) == 0) {
            return &g_cache.entries[i];
        }
    }
    return NULL;
}

/**
 * @brief Find LRU entry for eviction (must hold mutex)
 */
static module_cache_entry_t *cache_find_lru_locked(void)
{
    module_cache_entry_t *lru = NULL;
    int64_t oldest = INT64_MAX;

    /* First pass: find empty slot */
    for (int i = 0; i < CONFIG_AKIRA_MODULE_CACHE_SIZE; i++) {
        if (!g_cache.entries[i].used) {
            return &g_cache.entries[i];
        }
    }

    /* Second pass: find LRU with ref_count == 0 */
    for (int i = 0; i < CONFIG_AKIRA_MODULE_CACHE_SIZE; i++) {
        if (g_cache.entries[i].ref_count == 0 &&
            g_cache.entries[i].last_used_ms < oldest) {
            oldest = g_cache.entries[i].last_used_ms;
            lru = &g_cache.entries[i];
        }
    }

    /* Fallback: find any LRU */
    if (!lru) {
        oldest = INT64_MAX;
        for (int i = 0; i < CONFIG_AKIRA_MODULE_CACHE_SIZE; i++) {
            if (g_cache.entries[i].last_used_ms < oldest) {
                oldest = g_cache.entries[i].last_used_ms;
                lru = &g_cache.entries[i];
            }
        }
    }

    return lru;
}

wasm_module_t module_cache_lookup(const uint8_t *hash)
{
    if (!hash || !g_cache.initialized) return NULL;

    k_mutex_lock(&g_cache_mutex, K_FOREVER);

    module_cache_entry_t *entry = cache_find_locked(hash);
    if (entry) {
        entry->ref_count++;
        entry->last_used_ms = k_uptime_get();
        g_cache.stats.hits++;

        LOG_DBG("Module cache HIT (refs=%u, size=%u)",
                entry->ref_count, entry->binary_size);

        wasm_module_t mod = entry->module;
        k_mutex_unlock(&g_cache_mutex);
        return mod;
    }

    g_cache.stats.misses++;
    k_mutex_unlock(&g_cache_mutex);
    return NULL;
}

int module_cache_store(const uint8_t *hash, wasm_module_t module,
                       uint32_t binary_size, uint32_t load_time_ms)
{
    if (!hash || !module || !g_cache.initialized) return -EINVAL;

    k_mutex_lock(&g_cache_mutex, K_FOREVER);

    /* Check if already cached */
    module_cache_entry_t *existing = cache_find_locked(hash);
    if (existing) {
        existing->ref_count++;
        existing->last_used_ms = k_uptime_get();
        k_mutex_unlock(&g_cache_mutex);
        LOG_DBG("Module already cached, incremented ref_count");
        return 0;
    }

    /* Find slot (empty or LRU) */
    module_cache_entry_t *slot = cache_find_lru_locked();
    if (!slot) {
        k_mutex_unlock(&g_cache_mutex);
        LOG_ERR("Cannot find cache slot for eviction");
        return -ENOMEM;
    }

    /* Evict if occupied */
    if (slot->used) {
#ifdef CONFIG_AKIRA_WASM_RUNTIME
        if (slot->ref_count == 0 && slot->module) {
            LOG_INF("Evicting cached module (size=%u, age=%lldms)",
                    slot->binary_size,
                    k_uptime_get() - slot->last_used_ms);
            wasm_runtime_unload(slot->module);
        } else {
            LOG_WRN("Evicting module with %u active refs", slot->ref_count);
            /* Don't unload - instances still reference it */
        }
#endif
        g_cache.stats.evictions++;
    }

    /* Store new entry */
    memcpy(slot->hash, hash, 32);
    slot->module = module;
    slot->ref_count = 1;
    slot->load_time_ms = load_time_ms;
    slot->binary_size = binary_size;
    slot->last_used_ms = k_uptime_get();
    slot->used = true;

    g_cache.stats.total_load_time_ms += load_time_ms;

    k_mutex_unlock(&g_cache_mutex);

    LOG_INF("Module cached (size=%u, load=%ums)", binary_size, load_time_ms);
    return 0;
}

void module_cache_release(const uint8_t *hash)
{
    if (!hash || !g_cache.initialized) return;

    k_mutex_lock(&g_cache_mutex, K_FOREVER);

    module_cache_entry_t *entry = cache_find_locked(hash);
    if (entry && entry->ref_count > 0) {
        entry->ref_count--;
        LOG_DBG("Module cache release (refs=%u)", entry->ref_count);
    }

    k_mutex_unlock(&g_cache_mutex);
}

void module_cache_get_stats(module_cache_stats_t *stats)
{
    if (!stats) return;

    k_mutex_lock(&g_cache_mutex, K_FOREVER);
    memcpy(stats, &g_cache.stats, sizeof(*stats));
    k_mutex_unlock(&g_cache_mutex);
}

/* ===== Instance Map (pointer hash) ===== */

typedef struct {
    wasm_module_inst_t inst;
    int slot;
} instance_map_entry_t;

static struct {
    instance_map_entry_t buckets[INSTANCE_MAP_SIZE];
    bool initialized;
} g_inst_map = {0};

static struct k_spinlock g_inst_map_lock;

int instance_map_init(void)
{
    memset(&g_inst_map, 0, sizeof(g_inst_map));
    for (int i = 0; i < INSTANCE_MAP_SIZE; i++) {
        g_inst_map.buckets[i].inst = NULL;
        g_inst_map.buckets[i].slot = -1;
    }
    g_inst_map.initialized = true;
    LOG_INF("Instance map initialized (%d buckets)", INSTANCE_MAP_SIZE);
    return 0;
}

/**
 * @brief Hash a pointer to a bucket index
 *
 * Uses multiplicative hashing with a Knuth constant for
 * good distribution of pointer values.
 */
static inline uint32_t ptr_hash(const void *ptr)
{
    uintptr_t val = (uintptr_t)ptr;
    /* Knuth's multiplicative hash */
    val = (val >> 4) * 2654435761U;
    return (uint32_t)(val & INSTANCE_MAP_MASK);
}

int instance_map_put(wasm_module_inst_t inst, int slot)
{
    if (!inst || !g_inst_map.initialized) return -EINVAL;

    k_spinlock_key_t key = k_spin_lock(&g_inst_map_lock);

    uint32_t idx = ptr_hash(inst);

    /* Linear probing */
    for (int i = 0; i < INSTANCE_MAP_SIZE; i++) {
        uint32_t probe = (idx + i) & INSTANCE_MAP_MASK;
        if (g_inst_map.buckets[probe].inst == NULL ||
            g_inst_map.buckets[probe].inst == inst) {
            g_inst_map.buckets[probe].inst = inst;
            g_inst_map.buckets[probe].slot = slot;
            k_spin_unlock(&g_inst_map_lock, key);
            return 0;
        }
    }

    k_spin_unlock(&g_inst_map_lock, key);
    LOG_ERR("Instance map full");
    return -ENOMEM;
}

int instance_map_get(wasm_module_inst_t inst)
{
    if (!inst || !g_inst_map.initialized) return -1;

    k_spinlock_key_t key = k_spin_lock(&g_inst_map_lock);

    uint32_t idx = ptr_hash(inst);

    for (int i = 0; i < INSTANCE_MAP_SIZE; i++) {
        uint32_t probe = (idx + i) & INSTANCE_MAP_MASK;
        if (g_inst_map.buckets[probe].inst == inst) {
            int slot = g_inst_map.buckets[probe].slot;
            k_spin_unlock(&g_inst_map_lock, key);
            return slot;
        }
        if (g_inst_map.buckets[probe].inst == NULL) {
            break; /* Empty slot = not found */
        }
    }

    k_spin_unlock(&g_inst_map_lock, key);
    return -1;
}

void instance_map_remove(wasm_module_inst_t inst)
{
    if (!inst || !g_inst_map.initialized) return;

    k_spinlock_key_t key = k_spin_lock(&g_inst_map_lock);

    uint32_t idx = ptr_hash(inst);

    for (int i = 0; i < INSTANCE_MAP_SIZE; i++) {
        uint32_t probe = (idx + i) & INSTANCE_MAP_MASK;
        if (g_inst_map.buckets[probe].inst == inst) {
            g_inst_map.buckets[probe].inst = NULL;
            g_inst_map.buckets[probe].slot = -1;
            k_spin_unlock(&g_inst_map_lock, key);
            return;
        }
        if (g_inst_map.buckets[probe].inst == NULL) {
            break;
        }
    }

    k_spin_unlock(&g_inst_map_lock, key);
}
