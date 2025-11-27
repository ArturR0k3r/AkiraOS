/**
 * @file akira_input_api.c
 * @brief Input API implementation for WASM exports
 */

#include "akira_api.h"
#include "../drivers/akira_buttons.h"
#include "../drivers/akira_hal.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_input_api, LOG_LEVEL_INF);

static akira_input_callback_t g_input_callback = NULL;

// TODO: Add capability check before each API call
// TODO: Add debouncing logic
// TODO: Add long-press detection
// TODO: Add gesture support (swipe, tap patterns)

uint32_t akira_input_read_buttons(void)
{
	// TODO: Check CAP_INPUT_READ capability
	// TODO: Read from akira_buttons driver
	// TODO: Map hardware buttons to API button masks
	
#if AKIRA_PLATFORM_NATIVE_SIM
	return akira_sim_read_buttons();
#else
	// TODO: Read from GPIO
	return 0;
#endif
}

bool akira_input_button_pressed(uint32_t button)
{
	return (akira_input_read_buttons() & button) != 0;
}

void akira_input_set_callback(akira_input_callback_t callback)
{
	// TODO: Check CAP_INPUT_READ capability
	// TODO: Register with button interrupt system
	// TODO: Store callback per-container
	g_input_callback = callback;
	LOG_INF("Input callback registered: %p", callback);
}

// TODO: Internal function called by button ISR
void akira_input_notify(uint32_t buttons)
{
	if (g_input_callback) {
		g_input_callback(buttons);
	}
}
