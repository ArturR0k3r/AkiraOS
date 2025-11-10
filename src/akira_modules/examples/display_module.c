/*
 * SPDX-License-Identifier: Apache-2.0
 * 
 * Akira Display Module - Pre-built display control module
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <akira_module.h>
#include "../../src/drivers/display_ili9341.h"

LOG_MODULE_REGISTER(akira_display_module, CONFIG_LOG_DEFAULT_LEVEL);

static int display_module_init(void *user_data)
{
	LOG_INF("Display module initialized");
	return 0;
}

static int display_module_command(const char *command, void *data, 
                                   size_t len, void *user_data)
{
	if (strcmp(command, "clear") == 0) {
		ili9341_fill_screen(WHITE_COLOR);
		LOG_DBG("Display cleared");
		return 0;
	}
	else if (strcmp(command, "text") == 0 && data && len > 0) {
		/* Data format: x,y,text */
		char *str = (char *)data;
		int x = 10, y = 10;
		
		/* Parse coordinates if provided */
		char *comma1 = strchr(str, ',');
		if (comma1) {
			x = atoi(str);
			char *comma2 = strchr(comma1 + 1, ',');
			if (comma2) {
				y = atoi(comma1 + 1);
				str = comma2 + 1;
			}
		}
		
		ili9341_draw_text(x, y, str, BLACK_COLOR, FONT_7X10);
		LOG_DBG("Drew text at (%d,%d): %s", x, y, str);
		return 0;
	}
	else if (strcmp(command, "fill") == 0 && len == sizeof(uint16_t)) {
		uint16_t color = *(uint16_t *)data;
		ili9341_fill_screen(color);
		LOG_DBG("Filled screen with color 0x%04x", color);
		return 0;
	}
	else if (strcmp(command, "rect") == 0) {
		/* Data format: x,y,w,h,color */
		if (len >= 10) {
			uint16_t *params = (uint16_t *)data;
			ili9341_fill_rect(params[0], params[1], params[2], params[3], params[4]);
			return 0;
		}
		return -EINVAL;
	}
	
	LOG_WRN("Unknown display command: %s", command);
	return -ENOTSUP;
}

static int display_module_event(const char *event, void *data,
                                 size_t len, void *user_data)
{
	if (strcmp(event, "system_ready") == 0) {
		ili9341_draw_text(10, 10, "System Ready", GREEN_COLOR, FONT_11X18);
		return 0;
	}
	return 0;
}

AKIRA_MODULE_DEFINE(display,
                    AKIRA_MODULE_TYPE_DISPLAY,
                    display_module_init,
                    NULL,
                    display_module_command,
                    display_module_event,
                    NULL);
