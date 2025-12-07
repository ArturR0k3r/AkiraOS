/**
 * @file psram.h
 * @brief AkiraOS PSRAM (External SPI RAM) Management Interface
 *
 * Provides convenient API for allocating memory from external PSRAM
 * on ESP32-S3 N16R8 modules (8MB OPI PSRAM).
 *
 * Uses Zephyr's shared_multi_heap to manage PSRAM allocations with
 * SMH_REG_ATTR_EXTERNAL attribute.
 */

#ifndef AKIRA_KERNEL_PSRAM_H
#define AKIRA_KERNEL_PSRAM_H

#include "types.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*===========================================================================*/
/* Constants                                                                 */
/*===========================================================================*/

/** PSRAM total size (8MB for N16R8 module) */
#define AKIRA_PSRAM_SIZE_BYTES (8 * 1024 * 1024)

/** PSRAM usable heap size (configured in Kconfig) */
#ifdef CONFIG_ESP_SPIRAM_HEAP_SIZE
#define AKIRA_PSRAM_HEAP_SIZE CONFIG_ESP_SPIRAM_HEAP_SIZE
#else
#define AKIRA_PSRAM_HEAP_SIZE (4 * 1024 * 1024)
#endif

/*===========================================================================*/
/* PSRAM Statistics                                                          */
/*===========================================================================*/

/** PSRAM usage statistics */
typedef struct {
    size_t total_bytes;       /**< Total PSRAM available */
    size_t used_bytes;        /**< Currently allocated */
    size_t free_bytes;        /**< Available for allocation */
    size_t peak_usage;        /**< Peak memory usage */
    uint32_t alloc_count;     /**< Total allocations */
    uint32_t free_count;      /**< Total frees */
    uint32_t alloc_failures;  /**< Failed allocations */
} akira_psram_stats_t;

/*===========================================================================*/
/* PSRAM API                                                                 */
/*===========================================================================*/

/**
 * @brief Check if PSRAM is available on this platform
 * @return true if PSRAM is available, false otherwise
 */
bool akira_psram_available(void);

/**
 * @brief Get total PSRAM size
 * @return Total PSRAM size in bytes, or 0 if not available
 */
size_t akira_psram_get_size(void);

/**
 * @brief Get free PSRAM available
 * @return Free PSRAM in bytes
 */
size_t akira_psram_get_free(void);

/**
 * @brief Allocate memory from PSRAM
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 * 
 * @note Memory allocated from PSRAM is slower than internal SRAM.
 *       Use for large buffers, WASM heaps, frame buffers, etc.
 */
void *akira_psram_alloc(size_t size);

/**
 * @brief Allocate aligned memory from PSRAM
 * @param alignment Required alignment (must be power of 2)
 * @param size Number of bytes to allocate
 * @return Pointer to aligned memory, or NULL on failure
 */
void *akira_psram_aligned_alloc(size_t alignment, size_t size);

/**
 * @brief Allocate zeroed memory from PSRAM
 * @param count Number of elements
 * @param size Size of each element
 * @return Pointer to zeroed memory, or NULL on failure
 */
void *akira_psram_calloc(size_t count, size_t size);

/**
 * @brief Free memory allocated from PSRAM
 * @param ptr Pointer to memory to free (must be from akira_psram_alloc)
 */
void akira_psram_free(void *ptr);

/**
 * @brief Check if pointer is in PSRAM address range
 * @param ptr Pointer to check
 * @return true if pointer is in PSRAM, false otherwise
 */
bool akira_psram_ptr_is_psram(const void *ptr);

/**
 * @brief Get PSRAM statistics
 * @param stats Output statistics structure
 * @return 0 on success, negative on error
 */
int akira_psram_get_stats(akira_psram_stats_t *stats);

/**
 * @brief Print PSRAM status information
 */
void akira_psram_dump_stats(void);

/*===========================================================================*/
/* PSRAM Memory Pool API                                                     */
/*===========================================================================*/

/**
 * @brief Create a memory pool in PSRAM
 * @param name Pool name
 * @param size Pool size in bytes
 * @return Pool handle or NULL on failure
 *
 * @note Useful for creating dedicated PSRAM pools for specific subsystems
 *       like WASM runtime, frame buffers, etc.
 */
void *akira_psram_pool_create(const char *name, size_t size);

/**
 * @brief Destroy a PSRAM pool
 * @param pool Pool handle from akira_psram_pool_create
 */
void akira_psram_pool_destroy(void *pool);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_KERNEL_PSRAM_H */
