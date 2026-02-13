/**
 * @file akira_common_api.h
 * @brief Common API declarations for WASM exports
 */

#ifndef AKIRA_COMMON_API_H
#define AKIRA_COMMON_API_H

#include <wasm_export.h>
#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM native export functions */
int akira_native_log(wasm_exec_env_t exec_env, uint32_t level, char *message);
#endif

#endif /* AKIRA_COMMON_API_H */