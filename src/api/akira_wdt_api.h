/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AKIRA_WDT_API_H
#define AKIRA_WDT_API_H

#include <runtime/security.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/**
 * @brief WASM native: wdt_pet() -> 0 on success, negative on error
 *
 * Allows a WASM app to manually pet/feed the system watchdog, signalling
 * that the application is still alive.
 *
 * Capability: AKIRA_CAP_WDT ("wdt")
 */
int akira_native_wdt_pet(wasm_exec_env_t exec_env);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_WDT_API_H */
