/**
 * @file akira_memory_api.h
 * @brief Memory API declarations for WASM exports
 */

#ifndef AKIRA_MEMORY_API_H
#define AKIRA_MEMORY_API_H

#ifdef CONFIG_AKIRA_WASM_RUNTIME

#include <stdint.h>
#include <stddef.h>


#include <wasm_export.h>

/* Allocation header for quota tracking */
#define AKIRA_ALLOC_MAGIC 0xAA4B5241

#define AKIRA_ALLOC_MAGIC 0xAA4B5241  /* "AK1R" in hex-ish */

/* Allocation header to track size for quota accounting */
typedef struct {
    uint32_t magic;      /* Magic number for validation: 0xAK1RA */
    uint32_t size;       /* Allocated size (excluding header) */
    int32_t  app_slot;   /* App slot index for quota tracking */
} akira_alloc_header_t;

/* Core memory API functions (no security checks) */
void *akira_wasm_malloc(wasm_exec_env_t exec_env, size_t size);
void akira_wasm_free(wasm_exec_env_t exec_env, void *ptr);

/* WASM native export functions */
uint32_t akira_native_mem_alloc(wasm_exec_env_t exec_env, uint32_t size);
void akira_native_mem_free(wasm_exec_env_t exec_env, uint32_t ptr);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#endif /* AKIRA_MEMORY_API_H */