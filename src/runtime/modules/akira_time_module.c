/**
 * @file akira_time_module.c
 * @brief Modular WASM Time API for AkiraOS (WAMR)
 *
 * This module implements the time API exported to WASM apps.
 * It is registered as a separate native module with WAMR.
 */

#include "wasm_export.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_time_module, CONFIG_AKIRA_LOG_LEVEL);

static int64_t get_time_ms(wasm_exec_env_t exec_env) {
    return (int64_t)k_uptime_get();
}

static void sleep_ms(wasm_exec_env_t exec_env, int32_t ms) {
    if (ms > 0 && ms < 3600000) {
        k_msleep(ms);
    }
}

#ifdef CONFIG_AKIRA_WASM_API
#include <akira_native_registry.h>

static const NativeSymbol time_symbols[] = {
    EXPORT_WASM_API_WITH_SIG(get_time_ms, "()I"),
    EXPORT_WASM_API_WITH_SIG(sleep_ms, "(i)v"),
};

/* Optional: if WAMR rejects it, only these time imports are unavailable. */
AKIRA_NATIVE_API_DEFINE_FLAGS(akira_time_module_api, "akira_time", time_symbols,
                              AKIRA_NATIVE_API_OPTIONAL);
#endif

/* Deprecated since 1.6: the runtime registers the "akira_time" module through
 * the native API registry. Kept so existing callers keep linking. */
int akira_register_time_module(void) {
    return 0;
}
