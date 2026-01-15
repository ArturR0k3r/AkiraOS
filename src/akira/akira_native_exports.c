/**
 * @file akira_native_exports.c
 * @brief WASM native function registration
 *
 * Registers Akira APIs with OCRE so WASM apps can call them.
 * Wrapper functions are defined in their respective API files.
 */

#include "../api/akira_api.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_native_exports, LOG_LEVEL_INF);

extern int ocre_register_native_module(const char *module_name, NativeSymbol *symbols, int symbol_count);

int register_akira_native_module(void)
{
    NativeSymbol akira_symbols[] = {
        /* Display API */
        {"akira_display_clear", (void*)akira_display_clear_wasm, "(i)i", NULL},
        {"akira_display_pixel", (void*)akira_display_pixel_wasm, "(iii)i", NULL},
        {"akira_display_rect", (void*)akira_display_rect_wasm, "(iiiii)i", NULL},
        {"akira_display_get_size", (void*)akira_display_get_size_wasm, "(ii)i", NULL},

        /* Storage API */
        {"akira_storage_read", (void*)akira_storage_read_wasm, "($i)i", NULL},
        {"akira_storage_write", (void*)akira_storage_write_wasm, "($$i)i", NULL},
        {"akira_storage_delete", (void*)akira_storage_delete_wasm, "($)i", NULL},
        {"akira_storage_size", (void*)akira_storage_size_wasm, "($)i", NULL},

        /* Network API */
        {"akira_http_get", (void*)akira_http_get_wasm, "($i)i", NULL},
        {"akira_http_post", (void*)akira_http_post_wasm, "($$i)i", NULL},

        /* HID API */
        {"akira_hid_set_transport", (void*)akira_hid_set_transport_wasm, "(i)i", NULL},
        {"akira_hid_enable", (void*)akira_hid_enable_wasm, "()i", NULL},
        {"akira_hid_disable", (void*)akira_hid_disable_wasm, "()i", NULL},
        {"akira_hid_keyboard_type", (void*)akira_hid_keyboard_type_wasm, "($)i", NULL},
        {"akira_hid_keyboard_press", (void*)akira_hid_keyboard_press_wasm, "(i)i", NULL},
        {"akira_hid_keyboard_release", (void*)akira_hid_keyboard_release_wasm, "(i)i", NULL},
    };

    int count = (int)(sizeof(akira_symbols) / sizeof(akira_symbols[0]));

    /* Register under 'akira' module name */
    int ret = ocre_register_native_module("akira", akira_symbols, count);
    if (ret < 0) {
        LOG_ERR("Failed to register akira module: %d", ret);
        return ret;
    }

    LOG_INF("Registered %d Akira native functions", count);
    return 0;
}
