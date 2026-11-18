/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
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
 * Liveness signal from a WASM app.  Never feeds the hardware watchdog (that
 * would let an app mask a wedged system); returns 0 while the system
 * watchdog is active, -ENODEV otherwise.
 *
 * Capability: AKIRA_CAP_WDT ("wdt")
 * @stability stable
 * @since 1.4
 */
int akira_native_wdt_pet(wasm_exec_env_t exec_env);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_WDT_API_H */
