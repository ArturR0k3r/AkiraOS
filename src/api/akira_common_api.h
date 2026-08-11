/**
 * @file akira_common_api.h
 * @brief Common API declarations for WASM exports
 * @stability stable
 * @since 1.0
 */

#ifndef AKIRA_COMMON_API_H
#define AKIRA_COMMON_API_H

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif
#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM native export functions */
int akira_native_printf(wasm_exec_env_t exec_env, char *message);
int akira_native_delay(wasm_exec_env_t exec_env, uint32_t microseconds);
#endif

#endif /* AKIRA_COMMON_API_H */