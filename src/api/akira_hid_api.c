/**
 * @file akira_hid_api.c
 * @brief HID (Human Interface Device) API for WASM
 * 
 * Keyboard/mouse emulation exports for WASM apps.
 */

#include "akira_api.h"
#include "../connectivity/hid/hid_manager.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_hid_api, LOG_LEVEL_INF);

/* Transport configuration */
int akira_hid_set_transport(int transport)
{
    return hid_manager_set_transport((hid_transport_t)transport);
}

/* Enable/Disable HID */
int akira_hid_enable(void)
{
    return hid_manager_enable();
}

int akira_hid_disable(void)
{
    return hid_manager_disable();
}

/* Keyboard functions */
int akira_hid_keyboard_type(const char *str)
{
    if (!str)
        return -1;
    return hid_keyboard_type_string(str);
}

int akira_hid_keyboard_press(int key)
{
    return hid_keyboard_press((hid_key_code_t)key);
}

int akira_hid_keyboard_release(int key)
{
    return hid_keyboard_release((hid_key_code_t)key);
}

/*===========================================================================*/
/* WASM Wrappers (exported to OCRE)                                          */
/*===========================================================================*/

#include <wasm_export.h>

int akira_hid_set_transport_wasm(wasm_exec_env_t exec_env, int transport)
{
    (void)exec_env;
    return akira_hid_set_transport(transport);
}

int akira_hid_enable_wasm(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return akira_hid_enable();
}

int akira_hid_disable_wasm(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return akira_hid_disable();
}

int akira_hid_keyboard_type_wasm(wasm_exec_env_t exec_env, uint32_t str_ptr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    
    const char *str = (const char *)wasm_runtime_addr_app_to_native(module_inst, str_ptr);
    if (!str)
        return -1;
    
    return akira_hid_keyboard_type(str);
}

int akira_hid_keyboard_press_wasm(wasm_exec_env_t exec_env, int key)
{
    (void)exec_env;
    return akira_hid_keyboard_press(key);
}

int akira_hid_keyboard_release_wasm(wasm_exec_env_t exec_env, int key)
{
    (void)exec_env;
    return akira_hid_keyboard_release(key);
}
