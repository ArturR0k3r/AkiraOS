/**
 * @file mem_helper.h
 * @brief Memory allocation helper with PSRAM/SRAM fallback
 *
 * Provides a unified allocation API that attempts PSRAM first,
 * falling back to internal SRAM when PSRAM is unavailable.
 */

#ifndef AKIRA_MEM_HELPER_H
#define AKIRA_MEM_HELPER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Memory source indicator
 */
typedef enum {
    MEM_SOURCE_UNKNOWN = 0,
    MEM_SOURCE_PSRAM,      /**< Allocated from external PSRAM */
    MEM_SOURCE_SRAM,       /**< Allocated from internal SRAM */
} mem_source_t;

/**
 * @brief Allocate a buffer, preferring PSRAM when available
 *
 * Attempts allocation from PSRAM (via shared_multi_heap_alloc with
 * SMH_REG_ATTR_EXTERNAL). If PSRAM is unavailable or allocation fails,
 * falls back to k_malloc() from internal SRAM.
 *
 * @param size Number of bytes to allocate
 * @return Pointer to allocated buffer, or NULL on failure
 */
void *akira_malloc_buffer(size_t size);

/**
 * @brief Allocate a buffer with source tracking
 *
 * Same as akira_malloc_buffer() but reports which memory region
 * was used for the allocation.
 *
 * @param size Number of bytes to allocate
 * @param source Output: memory source indicator (can be NULL)
 * @return Pointer to allocated buffer, or NULL on failure
 */
void *akira_malloc_buffer_ex(size_t size, mem_source_t *source);

/**
 * @brief Free a buffer allocated with akira_malloc_buffer()
 *
 * Automatically detects whether the buffer is in PSRAM or SRAM
 * and calls the appropriate free function.
 *
 * @param ptr Pointer to buffer (NULL is safe)
 */
void akira_free_buffer(void *ptr);

/**
 * @brief Get the memory source for a pointer
 *
 * Determines if a pointer is in external PSRAM or internal SRAM.
 *
 * @param ptr Pointer to check
 * @return Memory source indicator
 */
mem_source_t akira_get_mem_source(void *ptr);

/**
 * @brief Allocate aligned buffer for DMA/flash operations
 *
 * Allocates a buffer with specified alignment, preferring PSRAM.
 * Useful for flash write buffers that require alignment.
 *
 * @param size Number of bytes to allocate
 * @param align Alignment requirement (must be power of 2)
 * @return Pointer to aligned buffer, or NULL on failure
 */
void *akira_malloc_aligned(size_t size, size_t align);

/**
 * @brief Free an aligned buffer
 *
 * @param ptr Pointer to buffer (NULL is safe)
 */
void akira_free_aligned(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MEM_HELPER_H */
