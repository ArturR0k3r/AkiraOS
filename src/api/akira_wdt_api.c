/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_wdt_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_wdt_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_wdt_api.c
 * @brief WASM-facing watchdog pet API.
 *
 * Thin capability-gated wrapper around akira_wdt_feed().
 * Requires AKIRA_CAP_WDT capability in the app manifest ("wdt").
 */

#include "akira_wdt_api.h"
#include <drivers/wdt/akira_wdt.h>
#include <runtime/security.h>
#include <errno.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME

int akira_native_wdt_pet(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_WDT, -EPERM);

    if (!akira_wdt_is_active()) {
        return -ENODEV;
    }

    akira_wdt_feed();
    LOG_DBG("WDT pet from WASM app");
    return 0;
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_WDT))
#include <akira_native_registry.h>

static const NativeSymbol akira_wdt_natives[] = {
    {"wdt_pet", (void *)akira_native_wdt_pet, "()i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_wdt_api, "env", akira_wdt_natives);
#endif
