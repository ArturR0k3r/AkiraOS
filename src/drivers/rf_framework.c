/**
 * @file rf_framework.c
 * @brief AkiraOS RF Framework Implementation
 */

#include "rf_framework.h"
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_rf_framework, LOG_LEVEL_INF);

#define MAX_RF_DRIVERS 8

static const struct akira_rf_driver *g_drivers[MAX_RF_DRIVERS];
static int g_driver_count = 0;

int rf_framework_init(void)
{
	memset(g_drivers, 0, sizeof(g_drivers));
	g_driver_count = 0;
	LOG_INF("RF framework initialized");
	return 0;
}

int rf_framework_register(const struct akira_rf_driver *driver)
{
	if (!driver || !driver->name) {
		return -1;
	}
	
	if (g_driver_count >= MAX_RF_DRIVERS) {
		LOG_ERR("Max RF drivers reached");
		return -2;
	}
	
	g_drivers[g_driver_count++] = driver;
	LOG_INF("Registered RF driver: %s", driver->name);
	
	return 0;
}

const struct akira_rf_driver *rf_framework_get_driver(rf_chip_t chip)
{
	for (int i = 0; i < g_driver_count; i++) {
		if (g_drivers[i]->type == chip) {
			return g_drivers[i];
		}
	}
	return NULL;
}
