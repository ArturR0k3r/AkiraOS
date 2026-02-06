/**
 * @file akira_input_api.c
 * @brief Input API implementation for WASM exports
 */

#include "akira_api.h"
#include "akira_input_api.h"
#include <runtime/security.h>
#include <drivers/platform_hal.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_input_api, LOG_LEVEL_INF);

static akira_input_callback_t g_input_callback = NULL;

int akira_input_read_buttons(void)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    return akira_sim_read_buttons();
#else
    /* TODO: Implement real GPIO-based reading */
    return 0;
#endif
}

int akira_input_button_pressed(uint32_t button)
{
    return (akira_input_read_buttons() & button) != 0;
}

int akira_input_set_callback(akira_input_callback_t callback)
{
    g_input_callback = callback;
    LOG_INF("Input callback registered: %p", callback);
}

void akira_input_notify(uint32_t buttons)
{
    if (g_input_callback)
    {
        g_input_callback(buttons);
    }
}


#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM Native export api */
int akira_native_input_read_buttons(wasm_exec_env_t exec_env)
{
    uint32_t cap_mask = akira_security_get_cap_mask(exec_env);
    AKIRA_CHECK_CAP_OR_RETURN(cap_mask, AKIRA_CAP_INPUT_READ, -EPERM);
    return (int)akira_input_read_buttons();
}

int akira_native_input_button_pressed(wasm_exec_env_t exec_env, uint32_t button)
{
    uint32_t cap_mask = akira_security_get_cap_mask(exec_env);
    AKIRA_CHECK_CAP_OR_RETURN(cap_mask, AKIRA_CAP_INPUT_READ, -EPERM);
    return (int)akira_input_button_pressed(button);
}

int akira_native_input_set_callback(wasm_exec_env_t exec_env, akira_input_callback_t callback)
{
    uint32_t cap_mask = akira_security_get_cap_mask(exec_env);
    AKIRA_CHECK_CAP_OR_RETURN(cap_mask, AKIRA_CAP_INPUT_WRITE, -EPERM);
    return akira_input_set_callback(callback);
}

int akira_native_input_notify(wasm_exec_env_t exec_env, uint32_t buttons)
{
    uint32_t cap_mask = akira_security_get_cap_mask(exec_env);
    AKIRA_CHECK_CAP_OR_RETURN(cap_mask, AKIRA_CAP_INPUT_WRITE, -EPERM);
    akira_input_notify(buttons);
    return 0;
}
#endif
