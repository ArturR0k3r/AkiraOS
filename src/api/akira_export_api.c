#include "akira_api.h"
#include <runtime/akira_runtime.h>
#include <runtime/security.h>
#include <zephyr/logging/log.h>
#include <stdbool.h>

#include <wasm_export.h>

bool akira_register_native_apis()
{   
    static NativeSymbol native_syms[] = {
        #ifdef CONFIG_AKIRA_WASM_API
        {"log", (void *)akira_native_log, "(i$)i", NULL},
        #endif

        #ifdef CONFIG_AKIRA_WASM_DISPLAY
        {"display_rect", (void *)akira_native_display_rect, "(iiiii)i", NULL},
        {"display_text", (void *)akira_native_display_text, "(ii$i)i", NULL},
        {"display_text_large", (void *)akira_native_display_text_large, "(ii$i)i", NULL},
        {"display_clear", (void *)akira_native_display_clear, "(i)i", NULL},
        {"display_pixel", (void *)akira_native_display_pixel, "(iii)i", NULL},
        #endif

        #ifdef CONFIG_AKIRA_WASM_INPUT
        // we should create another version of this API so it is more generic and can be used for other types of input (buttons, touch, etc)
        // and we can make this in sdk based on GPIO API or here as separate API        
        #endif

        #ifdef CONFIG_AKIRA_WASM_RF
        {"rf_set_frequency", (void *)akira_native_rf_set_frequency, "(i)i", NULL},
        {"rf_set_power", (void *)akira_native_rf_set_power, "(i)i", NULL},
        {"rf_get_rssi", (void *)akira_native_rf_get_rssi, "(i)i", NULL},
        {"rf_send", (void *)akira_native_rf_send, "(*i)i", NULL},
        #endif

        #ifdef CONFIG_AKIRA_WASM_SENSOR
        {"sensor_read", (void *)akira_native_sensor_read, "(i)i", NULL},
        #endif

        #ifdef CONFIG_AKIRA_WASM_MEMORY
        {"mem_alloc", (void *)akira_native_mem_alloc, "(i)i", NULL},
        {"mem_free", (void *)akira_native_mem_free, "(i)", NULL},
        #endif

        #ifdef CONFIG_AKIRA_WASM_BT_SHELL
        {"bt_shell_print", (void *)akira_native_bt_shell_send, "(i$)i", NULL},
        {"bt_shell_send_data", (void *)akira_native_bt_shell_send_data, "(i*i)i", NULL},
        {"bt_shell_is_ready", (void *)akira_native_bt_shell_is_ready, "()i", NULL},
        #endif
    };

    return wasm_runtime_register_natives("env", native_syms, sizeof(native_syms) / sizeof(NativeSymbol));
}