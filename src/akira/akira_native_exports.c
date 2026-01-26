/**
 * Akira native exports registration
 *
 * This module registers a small set of Akira native functions with WAMR
 * so WASM apps can call into Akira services (display, input) without
 * embedding Akira-specific symbols into the WAMR core.
 */

#include "api/akira_api.h"
#include <stddef.h>
#include "connectivity/hid/hid_manager.h"

#ifdef CONFIG_WAMR_ENABLE
#include <wasm_export.h>

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
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
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
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
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
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
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
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
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
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
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
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    const char *path = (const char *)wasm_runtime_addr_app_to_native(module_inst, path_ptr);
    if (!path)
        return -1;
    return akira_storage_delete(path);
}

static int akira_storage_size_wasm(wasm_exec_env_t exec_env, uint32_t path_ptr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    const char *path = (const char *)wasm_runtime_addr_app_to_native(module_inst, path_ptr);
    if (!path)
        return -1;
    return akira_storage_size(path);
}

static int akira_http_post_wasm(wasm_exec_env_t exec_env, uint32_t url_ptr, uint32_t data_ptr, int len)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
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

/* HID bindings for WASM apps */
static int akira_hid_set_transport_wasm(wasm_exec_env_t exec_env, int transport)
{
    (void)exec_env;
    return hid_manager_set_transport((hid_transport_t)transport);
}

static int akira_hid_enable_wasm(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return hid_manager_enable();
}

static int akira_hid_disable_wasm(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return hid_manager_disable();
}

static int akira_hid_keyboard_type_wasm(wasm_exec_env_t exec_env, uint32_t str_ptr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    const char *str = (const char *)wasm_runtime_addr_app_to_native(module_inst, str_ptr);
    if (!str)
        return -1;
    return hid_keyboard_type_string(str);
}

static int akira_hid_keyboard_press_wasm(wasm_exec_env_t exec_env, int key)
{
    (void)exec_env;
    return hid_keyboard_press((hid_key_code_t)key);
}

static int akira_hid_keyboard_release_wasm(wasm_exec_env_t exec_env, int key)
{
    (void)exec_env;
    return hid_keyboard_release((hid_key_code_t)key);
}

int register_akira_native_module(void)
{
    /* TODO: Implement WAMR native function registration
     * This would involve using WAMR's native function API to register
     * Akira functions for WASM apps to call.
     * For now, this is a placeholder - native functions can be
     * implemented directly in WAMR configuration or as separate modules
     */
    return 0;
}

#else /* CONFIG_WAMR_ENABLE not defined */

int register_akira_native_module(void)
{
    /* WAMR not enabled, stub implementation */
    return 0;
}

#endif /* CONFIG_WAMR_ENABLE */
