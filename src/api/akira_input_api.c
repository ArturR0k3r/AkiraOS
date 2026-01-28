/**
 * @file akira_input_api.c
 * @brief Input API implementation for WASM exports
 */

#include "akira_api.h"
#include <runtime/security.h>
#include <drivers/platform_hal.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_input_api, LOG_LEVEL_INF);

static akira_input_callback_t g_input_callback = NULL;

uint32_t akira_input_read_buttons(void)
{
    if (!akira_security_check("input.read"))
        return 0;

#if AKIRA_PLATFORM_NATIVE_SIM
    return akira_sim_read_buttons();
#else
    /* TODO: Implement real GPIO-based reading */
    return 0;
#endif
}

bool akira_input_button_pressed(uint32_t button)
{
    return (akira_input_read_buttons() & button) != 0;
}

void akira_input_set_callback(akira_input_callback_t callback)
{
    if (!akira_security_check("input.read"))
        return;

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

/* WASM native entry used by WAMR (registered in runtime) */
int akira_native_input_read_buttons(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return (int)akira_input_read_buttons();
}
