/*
 * SPDX-License-Identifier: Apache-2.0
 * 
 * Akira Button Module - Pre-built input/button control module
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <akira_module.h>
#include "../../src/shell/akira_shell.h"

LOG_MODULE_REGISTER(akira_button_module, CONFIG_LOG_DEFAULT_LEVEL);

static int button_module_init(void *user_data)
{
	LOG_INF("Button module initialized");
	return 0;
}

static int button_module_command(const char *command, void *data,
                                  size_t len, void *user_data)
{
	if (strcmp(command, "read") == 0) {
		if (data && len >= sizeof(uint32_t)) {
			uint32_t *buttons = (uint32_t *)data;
			*buttons = shell_read_buttons();
			LOG_DBG("Read button state: 0x%08x", *buttons);
			return 0;
		}
		return -EINVAL;
	}
	else if (strcmp(command, "wait") == 0) {
		/* Wait for any button press */
		uint32_t initial = shell_read_buttons();
		while (shell_read_buttons() == initial) {
			k_sleep(K_MSEC(10));
		}
		if (data && len >= sizeof(uint32_t)) {
			*(uint32_t *)data = shell_read_buttons();
		}
		return 0;
	}
	
	LOG_WRN("Unknown button command: %s", command);
	return -ENOTSUP;
}

static int button_module_event(const char *event, void *data,
                                size_t len, void *user_data)
{
	/* Button module can generate events on button press */
	/* This would be implemented with interrupt handlers */
	return 0;
}

AKIRA_MODULE_DEFINE(buttons,
                    AKIRA_MODULE_TYPE_INPUT,
                    button_module_init,
                    NULL,
                    button_module_command,
                    button_module_event,
                    NULL);
