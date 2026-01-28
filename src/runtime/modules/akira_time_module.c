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

static NativeSymbol time_symbols[] = {
    EXPORT_WASM_API_WITH_SIG(get_time_ms, "()I"),
    EXPORT_WASM_API_WITH_SIG(sleep_ms, "(i)v"),
};

int akira_register_time_module(void) {
    int count = sizeof(time_symbols) / sizeof(NativeSymbol);
    if (!wasm_runtime_register_natives("akira_time", time_symbols, count)) {
        LOG_ERR("Failed to register time module");
        return -1;
    }
    LOG_INF("AkiraOS time module registered");
    return 0;
}
