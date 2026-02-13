/**
 * @file runtime_cache.h
 * @brief WASM Module Cache & Instance Pool for AkiraOS Runtime
 *
 * Provides performance optimizations for the WASM runtime:
 * - Module cache: avoids reloading identical WASM binaries (SHA-256 key)
 * - Instance pool: pre-allocated exec environments for fast start
 * - O(1) module instance → slot lookup via hash map
 *
 * Memory layout: cache entries are pre-allocated in BSS, no dynamic
 * allocation on the hot path.
 */

#ifndef AKIRA_RUNTIME_CACHE_H
#define AKIRA_RUNTIME_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <zephyr/kernel.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#else
typedef void *wasm_module_t;
typedef void *wasm_module_inst_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Configuration ===== */

/** Maximum cached modules (trade RAM for performance) */
#ifndef CONFIG_AKIRA_MODULE_CACHE_SIZE
#define CONFIG_AKIRA_MODULE_CACHE_SIZE 4
#endif

/** Hash map size for instance→slot lookup (must be power of 2) */
#define INSTANCE_MAP_SIZE 16
#define INSTANCE_MAP_MASK (INSTANCE_MAP_SIZE - 1)

/* ===== Module Cache ===== */

/** Cached module entry */
typedef struct {
    bool used;
    uint8_t hash[32];           /**< SHA-256 of WASM binary */
    wasm_module_t module;       /**< Loaded WASM module */
    uint32_t ref_count;         /**< Number of active instances */
    uint32_t load_time_ms;      /**< Time taken to load (for profiling) */
    uint32_t binary_size;       /**< Original binary size */
    int64_t last_used_ms;       /**< Last access timestamp */
} module_cache_entry_t;

/** Module cache statistics */
typedef struct {
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
    uint32_t total_load_time_ms;
} module_cache_stats_t;

/**
 * @brief Initialize the module cache
 * @return 0 on success
 */
int module_cache_init(void);

/**
 * @brief Look up a cached module by binary hash
 *
 * @param hash  SHA-256 hash of WASM binary (32 bytes)
 * @return Cached module, or NULL if not found
 */
wasm_module_t module_cache_lookup(const uint8_t *hash);

/**
 * @brief Store a module in the cache
 *
 * If cache is full, evicts the least recently used entry.
 *
 * @param hash          SHA-256 hash of WASM binary
 * @param module        Loaded WASM module
 * @param binary_size   Original binary size
 * @param load_time_ms  Time taken to load
 * @return 0 on success
 */
int module_cache_store(const uint8_t *hash, wasm_module_t module,
                       uint32_t binary_size, uint32_t load_time_ms);

/**
 * @brief Release a cached module reference
 *
 * Decrements reference count. Module is not unloaded until
 * evicted and ref_count reaches 0.
 *
 * @param hash  SHA-256 hash of the module
 */
void module_cache_release(const uint8_t *hash);

/**
 * @brief Get cache statistics
 * @param stats  Output statistics
 */
void module_cache_get_stats(module_cache_stats_t *stats);

/* ===== Instance Map (O(1) lookup) ===== */

/**
 * @brief Initialize the instance map
 * @return 0 on success
 */
int instance_map_init(void);

/**
 * @brief Map a module instance to an app slot
 *
 * @param inst  WASM module instance
 * @param slot  App slot index
 * @return 0 on success
 */
int instance_map_put(wasm_module_inst_t inst, int slot);

/**
 * @brief Look up app slot from module instance
 *
 * O(1) average case using pointer hashing.
 *
 * @param inst  WASM module instance
 * @return Slot index, or -1 if not found
 */
int instance_map_get(wasm_module_inst_t inst);

/**
 * @brief Remove a module instance from the map
 *
 * @param inst  WASM module instance
 */
void instance_map_remove(wasm_module_inst_t inst);

/* ===== Runtime Profiling ===== */

/** Per-instance execution statistics */
typedef struct {
    uint64_t total_exec_time_us;    /**< Total execution time in microseconds */
    uint32_t call_count;            /**< Total function calls */
    uint32_t trap_count;            /**< WASM trap count */
    int64_t last_exec_start_ms;     /**< Start of current execution */
    uint32_t peak_memory_bytes;     /**< Peak memory usage */
} runtime_perf_stats_t;

/**
 * @brief Record execution start for profiling
 * @param stats  Performance stats struct
 */
static inline void perf_exec_begin(runtime_perf_stats_t *stats)
{
    if (stats) {
        stats->last_exec_start_ms = k_uptime_get();
    }
}

/**
 * @brief Record execution end and accumulate time
 * @param stats  Performance stats struct
 */
static inline void perf_exec_end(runtime_perf_stats_t *stats)
{
    if (stats && stats->last_exec_start_ms > 0) {
        int64_t elapsed = k_uptime_get() - stats->last_exec_start_ms;
        stats->total_exec_time_us += (uint64_t)(elapsed * 1000);
        stats->call_count++;
        stats->last_exec_start_ms = 0;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_RUNTIME_CACHE_H */
