/**
 * Akira native exports registration
 *
 * This module registers a small set of Akira native functions with OCRE's
 * runtime using `ocre_register_native_module()` so WASM apps can call into
 * Akira services (display, input) without embedding Akira-specific symbols
 * into the OCRE core.
 */

#include "api/akira_api.h"
#include <wasm_export.h>
#include <stddef.h>

/* OCRE registration API */
extern int ocre_register_native_module(const char *module_name, NativeSymbol *symbols, int symbol_count);

/* WASM wrappers */
static int akira_display_clear_wasm(wasm_exec_env_t exec_env, int color)
{
    (void)exec_env;
    akira_display_clear((uint16_t)color);
    return 0;
}

static int akira_display_pixel_wasm(wasm_exec_env_t exec_env, int x, int y, int color)
{
    (void)exec_env;
    akira_display_pixel(x, y, (uint16_t)color);
    return 0;
}

static int akira_display_flush_wasm(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    akira_display_flush();
    return 0;
}

static int akira_input_read_buttons_wasm(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return (int)akira_input_read_buttons();
}

int register_akira_native_module(void)
{
    NativeSymbol akira_symbols[] = {
        {"akira_display_clear", akira_display_clear_wasm, "(i)i", NULL},
        {"akira_display_pixel", akira_display_pixel_wasm, "(iii)i", NULL},
        {"akira_display_flush", akira_display_flush_wasm, "()i", NULL},
        {"akira_input_read_buttons", akira_input_read_buttons_wasm, "()i", NULL},
    };

    int count = (int)(sizeof(akira_symbols) / sizeof(akira_symbols[0]));
    return ocre_register_native_module("akira", akira_symbols, count);
}
