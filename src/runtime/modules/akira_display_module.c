/**
 * @file modules/akira_display_module.c
 * @brief Modular WASM Display API for AkiraOS (WAMR)
 *
 * This module implements the display API exported to WASM apps.
 * It is registered as a separate native module with WAMR.
 */

#include "wasm_export.h"
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_display_module, CONFIG_AKIRA_LOG_LEVEL);

static int32_t display_write(wasm_exec_env_t exec_env,
                            int32_t x, int32_t y, int32_t w, int32_t h,
                            const uint8_t *buffer, int32_t size)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint32_t buf_off = (uint32_t)(uintptr_t)buffer;
    uint32_t len = (uint32_t)size;
    if (!wasm_runtime_validate_app_addr(module_inst, buf_off, len)) {
        LOG_ERR("Invalid buffer in display_write");
        return -1;
    }

    // TODO: Implement actual display logic (copy from app memory into framebuffer)
    LOG_DBG("Display write: x=%d y=%d w=%d h=%d size=%d", x, y, w, h, size);
    return 0;
}

static NativeSymbol display_symbols[] = {
    EXPORT_WASM_API_WITH_SIG(display_write, "(iii*~)i"),
};

int akira_register_display_module(void) {
    int count = sizeof(display_symbols) / sizeof(NativeSymbol);
    if (!wasm_runtime_register_natives("akira_display", display_symbols, count)) {
        LOG_ERR("Failed to register display module");
        return -1;
    }
    LOG_INF("AkiraOS display module registered");
    return 0;
}
