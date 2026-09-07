/**
 * @file mem_helper.h
 * @brief Memory allocation helper with PSRAM/SRAM fallback
 *
 * Provides a unified allocation API that attempts PSRAM first,
 * falling back to internal SRAM when PSRAM is unavailable.
 * @stability stable
 * @since 1.2
 */

#ifndef AKIRA_MEM_HELPER_H
#define AKIRA_MEM_HELPER_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

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
 * @defgroup akira_bulk_bss PSRAM section placement
 * @{
 *
 * Use AKIRA_BULK_BSS on large static arrays that must not live in internal
 * DRAM.  On boards with PSRAM (CONFIG_AKIRA_PSRAM=y) the variable lands in
 * .ext_ram.bss (external RAM).  On boards without PSRAM it falls back to
 * normal BSS — the Kconfig defaults for those boards must be small enough
 * to fit in internal DRAM.
 *
 * NEVER use this (or any PSRAM section) for a thread stack.  PSRAM sits on
 * SPI0 alongside the flash, so a flash write/erase — the LittleFS garbage
 * collector inside lfs_file_open, for one — disables the SPI0 cache and the
 * CPU can no longer read its own stack frames: hard fault or dead freeze.
 * The Zephyr port makes this worse than it sounds under CONFIG_SMP, because
 * spi_flash_disable_interrupts_caches_and_other_cpu() only locks interrupts
 * on the calling core and never stalls the other one, so any thread on the
 * other core is exposed too, not just the one doing the flash access.  See
 * the thread-pool comment in src/runtime/akira_runtime.c.
 *
 * Give the variable no initializer, not even `= {0}`: .ext_ram.bss lives in a
 * NOLOAD output section, so an explicit initializer only turns the section
 * PROGBITS (which then collides with the next uninitialized variable placed
 * there) without ever being loaded.  The PSRAM init memsets the region.
 *
 * Note the placement: with an inline `struct { ... }` type the attribute has
 * to follow the declarator (`} g_table AKIRA_BULK_BSS;`) or GCC binds it to
 * the type and drops it ("'section' attribute does not apply to types").
 *
 * Example:
 *   static my_big_struct_t AKIRA_BULK_BSS g_table[CONFIG_MY_TABLE_SIZE];
 */
#if defined(CONFIG_AKIRA_PSRAM)
#define AKIRA_BULK_BSS __attribute__((section(".ext_ram.bss"), aligned(4)))
#else
#define AKIRA_BULK_BSS  /**< no-op on non-PSRAM targets */
#endif

/**
 * @brief K_MSGQ_DEFINE with its ring buffer in PSRAM
 *
 * Statically initialized exactly like K_MSGQ_DEFINE, so there is no
 * k_msgq_init() ordering to get right — only the backing buffer moves.
 * Safe for any queue whose messages are plain data (no DMA target).
 */
#define AKIRA_BULK_MSGQ_DEFINE(q_name, q_msg_size, q_max_msgs, q_align)	\
	static char AKIRA_BULK_BSS __aligned(q_align)			\
		_k_fifo_buf_##q_name[(q_max_msgs) * (q_msg_size)];	\
	STRUCT_SECTION_ITERABLE(k_msgq, q_name) =			\
		Z_MSGQ_INITIALIZER(q_name, _k_fifo_buf_##q_name,	\
				   (q_msg_size), (q_max_msgs))
/** @} */

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
