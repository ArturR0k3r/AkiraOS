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

static int akira_display_rect_wasm(wasm_exec_env_t exec_env, int x, int y, int w, int h, int color)
{
    (void)exec_env;
    akira_display_rect(x, y, w, h, (uint16_t)color);
    return 0;
}

static int akira_display_text_wasm(wasm_exec_env_t exec_env, int x, int y, uint32_t text_ptr, int color)
{
    WASMModuleInstanceCommon *module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    const char *text = (const char *)wasm_runtime_addr_app_to_native(module_inst, text_ptr);
    if (!text)
        return -1;
    akira_display_text(x, y, text, (uint16_t)color);
    return 0;
}

static int akira_display_get_size_wasm(wasm_exec_env_t exec_env, uint32_t width_ptr, uint32_t height_ptr)
{
    WASMModuleInstanceCommon *module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    int *w = (int *)wasm_runtime_addr_app_to_native(module_inst, width_ptr);
    int *h = (int *)wasm_runtime_addr_app_to_native(module_inst, height_ptr);
    if (!w || !h)
        return -1;
    akira_display_get_size(w, h);
    return 0;
}

static int akira_storage_read_wasm(wasm_exec_env_t exec_env, uint32_t path_ptr, uint32_t buf_ptr, int len)
{
    WASMModuleInstanceCommon *module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    const char *path = (const char *)wasm_runtime_addr_app_to_native(module_inst, path_ptr);
    void *buf = (void *)wasm_runtime_addr_app_to_native(module_inst, buf_ptr);
    if (!path || !buf)
        return -1;
    return akira_storage_read(path, buf, (size_t)len);
}

static int akira_storage_write_wasm(wasm_exec_env_t exec_env, uint32_t path_ptr, uint32_t data_ptr, int len)
{
    WASMModuleInstanceCommon *module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    const char *path = (const char *)wasm_runtime_addr_app_to_native(module_inst, path_ptr);
    const void *data = (const void *)wasm_runtime_addr_app_to_native(module_inst, data_ptr);
    if (!path || !data)
        return -1;
    return akira_storage_write(path, data, (size_t)len);
}

static int akira_http_get_wasm(wasm_exec_env_t exec_env, uint32_t url_ptr, uint32_t buf_ptr, int max_len)
{
    WASMModuleInstanceCommon *module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    const char *url = (const char *)wasm_runtime_addr_app_to_native(module_inst, url_ptr);
    void *buf = (void *)wasm_runtime_addr_app_to_native(module_inst, buf_ptr);
    if (!url || !buf)
        return -1;
    return akira_http_get(url, buf, (size_t)max_len);
}

static int akira_storage_delete_wasm(wasm_exec_env_t exec_env, uint32_t path_ptr)
{
    WASMModuleInstanceCommon *module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    const char *path = (const char *)wasm_runtime_addr_app_to_native(module_inst, path_ptr);
    if (!path)
        return -1;
    return akira_storage_delete(path);
}

static int akira_storage_size_wasm(wasm_exec_env_t exec_env, uint32_t path_ptr)
{
    WASMModuleInstanceCommon *module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    const char *path = (const char *)wasm_runtime_addr_app_to_native(module_inst, path_ptr);
    if (!path)
        return -1;
    return akira_storage_size(path);
}

static int akira_http_post_wasm(wasm_exec_env_t exec_env, uint32_t url_ptr, uint32_t data_ptr, int len)
{
    WASMModuleInstanceCommon *module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    const char *url = (const char *)wasm_runtime_addr_app_to_native(module_inst, url_ptr);
    const void *data = (const void *)wasm_runtime_addr_app_to_native(module_inst, data_ptr);
    if (!url || !data)
        return -1;
    return akira_http_post(url, data, (size_t)len);
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
        {"akira_display_rect", akira_display_rect_wasm, "(iiiii)i", NULL},
        {"akira_display_text", akira_display_text_wasm, "(ii$i)i", NULL},
        {"akira_display_get_size", akira_display_get_size_wasm, "(ii)i", NULL},

        {"akira_storage_read", akira_storage_read_wasm, "($i)i", NULL},
        {"akira_storage_write", akira_storage_write_wasm, "($$i)i", NULL},
        {"akira_storage_delete", akira_storage_delete_wasm, "($)i", NULL},
        {"akira_storage_size", akira_storage_size_wasm, "($)i", NULL},

        {"akira_http_get", akira_http_get_wasm, "($i)i", NULL},
        {"akira_http_post", akira_http_post_wasm, "($$i)i", NULL},

    };

    int count = (int)(sizeof(akira_symbols) / sizeof(akira_symbols[0]));
    return ocre_register_native_module("akira", akira_symbols, count);
}
