/*
 * SPDX-License-Identifier: Apache-2.0
 * 
 * Akira Module Manager - High-level module management
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "akira_module.h"

LOG_MODULE_REGISTER(akira_module_manager, CONFIG_LOG_DEFAULT_LEVEL);

static K_THREAD_STACK_DEFINE(module_manager_stack, CONFIG_AKIRA_MODULE_THREAD_STACK_SIZE);
static struct k_thread module_manager_thread;
static bool manager_running = false;

static void module_manager_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("Akira module manager thread started");

	while (manager_running) {
		/* Module management tasks */
		/* Future: Health checks, watchdog, auto-recovery */
		k_sleep(K_SECONDS(1));
	}

	LOG_INF("Akira module manager thread stopped");
}

int akira_module_start_comm(void)
{
	if (manager_running) {
		LOG_WRN("Module manager already running");
		return 0;
	}

	manager_running = true;

	k_thread_create(&module_manager_thread, module_manager_stack,
			K_THREAD_STACK_SIZEOF(module_manager_stack),
			module_manager_thread_fn,
			NULL, NULL, NULL,
			CONFIG_AKIRA_MODULE_THREAD_PRIORITY, 0, K_NO_WAIT);

	k_thread_name_set(&module_manager_thread, "akira_module_mgr");

	LOG_INF("Akira module manager started");
	return 0;
}

int akira_module_stop_comm(void)
{
	if (!manager_running) {
		return 0;
	}

	manager_running = false;
	k_thread_join(&module_manager_thread, K_FOREVER);

	LOG_INF("Akira module manager stopped");
	return 0;
}
