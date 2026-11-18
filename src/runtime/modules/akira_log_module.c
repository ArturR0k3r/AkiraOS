/**
 * @file akira_log_module.c
 * @brief Modular WASM Logging API for AkiraOS (WAMR)
 *
 * This module implements the logging API exported to WASM apps.
 * It is registered as a separate native module with WAMR.
 */

#include "wasm_export.h"
#include <zephyr/logging/log.h>
#include <api/akira_common_api.h>

LOG_MODULE_REGISTER(akira_log_module, CONFIG_AKIRA_LOG_LEVEL);

/*
 * "($)v": WAMR has already checked that the whole string lies in the app's
 * memory and passes a native pointer (it is not an app offset). Output goes
 * through the app console, which cuts lines and rate-limits each app, so an
 * app logging in a loop can't drown the console.
 */
#define LOG_MAX_SCAN 640 /* characters looked at per call */

static size_t log_strlen(const char *s)
{
    size_t n = 0;

    while (n < LOG_MAX_SCAN && s[n] != '\0') {
        n++;
    }
    return n;
}

static void log_debug(wasm_exec_env_t exec_env, const char *message) {
    if (message) {
        akira_app_log_write(exec_env, LOG_LEVEL_DBG, message, log_strlen(message));
    }
}

static void log_info(wasm_exec_env_t exec_env, const char *message) {
    if (message) {
        akira_app_log_write(exec_env, LOG_LEVEL_INF, message, log_strlen(message));
    }
}

static void log_error(wasm_exec_env_t exec_env, const char *message) {
    if (message) {
        akira_app_log_write(exec_env, LOG_LEVEL_ERR, message, log_strlen(message));
    }
}

#ifdef CONFIG_AKIRA_WASM_API
#include <akira_native_registry.h>

static const NativeSymbol log_symbols[] = {
    EXPORT_WASM_API_WITH_SIG(log_debug, "($)v"),
    EXPORT_WASM_API_WITH_SIG(log_info,  "($)v"),
    EXPORT_WASM_API_WITH_SIG(log_error, "($)v"),
};

/* Optional: if WAMR rejects it, only these log imports are unavailable. */
AKIRA_NATIVE_API_DEFINE_FLAGS(akira_log_module_api, "akira_log", log_symbols,
                              AKIRA_NATIVE_API_OPTIONAL);
#endif

/* Deprecated since 1.6: the runtime registers the "akira_log" module through the
 * native API registry. Kept so existing callers keep linking. */
int akira_register_log_module(void) {
    return 0;
}
