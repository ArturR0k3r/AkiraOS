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
 * @brief Place a variable in external-RAM noinit
 *
 * Same idea as AKIRA_BULK_BSS, but lands in .ext_ram_noinit — the PSRAM
 * region the boot code does *not* zero.  Use it for memory whose contents
 * are meaningless before first use (thread stacks); AKIRA_BULK_BSS stays
 * the right choice for anything that must read back as zero.
 */
#if defined(CONFIG_AKIRA_PSRAM)
#define AKIRA_BULK_NOINIT __attribute__((section(".ext_ram_noinit.akira")))
#else
#define AKIRA_BULK_NOINIT  /**< no-op on non-PSRAM targets */
#endif

/**
 * @brief K_THREAD_STACK_DEFINE that puts the stack in PSRAM
 *
 * A thread stack in external RAM is only safe when the thread itself never
 * performs an internal-flash operation: esp_flash write/erase (and the NVS /
 * littlefs / settings paths that reach it) run with the cache off, which
 * makes PSRAM — including that thread's own stack — unreadable for the
 * duration.  Interrupts are disabled and the second CPU is halted across
 * that window, so a thread that only does sockets, SPI or radio work is
 * never scheduled while the cache is down.
 *
 * The same trade-off is already made one level down: with
 * CONFIG_ESP32_WIFI_NET_ALLOC_SPIRAM=y the SoC linker script puts every
 * libsubsys__net*.a / libdrivers__wifi.a stack in this region.
 *
 * @param sym  Thread stack symbol name
 * @param size Size of the stack memory region
 */
#if !defined(CONFIG_AKIRA_PSRAM)
#define AKIRA_BULK_STACK_DEFINE(sym, size) K_THREAD_STACK_DEFINE(sym, size)
#elif defined(CONFIG_USERSPACE)
#define AKIRA_BULK_STACK_DEFINE(sym, size) \
	Z_THREAD_STACK_DEFINE_IN(sym, size, AKIRA_BULK_NOINIT)
#else
/* Without CONFIG_USERSPACE, Zephyr aliases K_THREAD_STACK_DEFINE onto the
 * kernel-stack macros, and the Z_THREAD_STACK_* helpers are not defined. */
#define AKIRA_BULK_STACK_DEFINE(sym, size) \
	Z_KERNEL_STACK_DEFINE_IN(sym, size, AKIRA_BULK_NOINIT)
#endif

/**
 * @brief K_THREAD_DEFINE with its stack in PSRAM
 *
 * Identical to K_THREAD_DEFINE — same static thread object, same automatic
 * start at boot — except the stack is placed by AKIRA_BULK_STACK_DEFINE.
 * The same "no internal-flash access from this thread" rule applies.
 */
#define AKIRA_BULK_THREAD_DEFINE(name, stack_size, entry, p1, p2, p3,	\
				 prio, options, delay)			\
	AKIRA_BULK_STACK_DEFINE(_k_thread_stack_##name, stack_size);	\
	Z_THREAD_COMMON_DEFINE(name, stack_size, entry, p1, p2, p3,	\
			       prio, options, delay)

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
