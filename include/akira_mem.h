/**
 * @file akira_mem.h
 * @brief Large-buffer allocation that prefers external RAM
 *
 * On boards with PSRAM the buffer comes from external RAM, keeping internal
 * SRAM (often the binding constraint on ESP32-S3) free for stacks and drivers;
 * otherwise from the system heap. Use for buffers of a few hundred bytes and
 * up: packages, certificates, download chunks.
 *
 * @stability experimental
 * @since 1.7
 */

#ifndef AKIRA_MEM_H
#define AKIRA_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Allocate @p size bytes, PSRAM first. Returns NULL when out of memory. */
void *akira_mem_alloc(size_t size);

/** Free a buffer from akira_mem_alloc(). NULL is ignored. */
void akira_mem_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MEM_H */
