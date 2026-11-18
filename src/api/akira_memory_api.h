/**
 * @file akira_memory_api.h
 * @brief Memory API declarations for WASM exports
 * @stability stable
 * @since 1.2
 */

#ifndef AKIRA_MEMORY_API_H
#define AKIRA_MEMORY_API_H

#ifdef CONFIG_AKIRA_WASM_RUNTIME

#include <stdint.h>
#include <stddef.h>


#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

/*
 * Core memory API functions (no security checks).
 *
 * Host memory allocated on behalf of an app: counted against its quota,
 * never internal DRAM on boards with PSRAM, and freed when the app ends
 * (pointers into it must be dropped by then). akira_wasm_free() accepts only
 * pointers akira_wasm_malloc() returned and not yet freed.
 */
void *akira_wasm_malloc(wasm_exec_env_t exec_env, size_t size);
void akira_wasm_free(wasm_exec_env_t exec_env, void *ptr);

/* WASM native export functions */
uint32_t akira_native_mem_alloc(wasm_exec_env_t exec_env, uint32_t size);
void akira_native_mem_free(wasm_exec_env_t exec_env, uint32_t ptr);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#endif /* AKIRA_MEMORY_API_H */
