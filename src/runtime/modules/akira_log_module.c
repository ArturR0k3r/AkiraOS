/**
 * @file akira_log_module.c
 * @brief Modular WASM Logging API for AkiraOS (WAMR)
 *
 * This module implements the logging API exported to WASM apps.
 * It is registered as a separate native module with WAMR.
 */

#include "wasm_export.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_log_module, CONFIG_AKIRA_LOG_LEVEL);

static void log_debug(wasm_exec_env_t exec_env, const char *message) {
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint32_t off = (uint32_t)(uintptr_t)message;
    if (!wasm_runtime_validate_app_str_addr(module_inst, off)) {
        LOG_ERR("Invalid string address in log_debug");
        return;
    }
    const char *s = (const char *)wasm_runtime_addr_app_to_native(module_inst, off);
    LOG_DBG("WASM: %s", s ? s : "(invalid)");
}

static void log_info(wasm_exec_env_t exec_env, const char *message) {
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint32_t off = (uint32_t)(uintptr_t)message;
    if (!wasm_runtime_validate_app_str_addr(module_inst, off)) {
        LOG_ERR("Invalid string address in log_info");
        return;
    }
    const char *s = (const char *)wasm_runtime_addr_app_to_native(module_inst, off);
    LOG_INF("WASM: %s", s ? s : "(invalid)");
}

static void log_error(wasm_exec_env_t exec_env, const char *message) {
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint32_t off = (uint32_t)(uintptr_t)message;
    if (!wasm_runtime_validate_app_str_addr(module_inst, off)) {
        LOG_ERR("Invalid string address in log_error");
        return;
    }
    const char *s = (const char *)wasm_runtime_addr_app_to_native(module_inst, off);
    LOG_ERR("WASM: %s", s ? s : "(invalid)");
}

static NativeSymbol log_symbols[] = {
    EXPORT_WASM_API_WITH_SIG(log_debug, "($)v"),
    EXPORT_WASM_API_WITH_SIG(log_info,  "($)v"),
    EXPORT_WASM_API_WITH_SIG(log_error, "($)v"),
};

int akira_register_log_module(void) {
    int count = sizeof(log_symbols) / sizeof(NativeSymbol);
    if (!wasm_runtime_register_natives("akira_log", log_symbols, count)) {
        LOG_ERR("Failed to register log module");
        return -1;
    }
    LOG_INF("AkiraOS log module registered");
    return 0;
}
