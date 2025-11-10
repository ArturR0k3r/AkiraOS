/*
 * SPDX-License-Identifier: Apache-2.0
 * 
 * Akira Module Core Implementation
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/slist.h>
#include <string.h>
#include "akira_module.h"

LOG_MODULE_REGISTER(akira_module_core, CONFIG_LOG_DEFAULT_LEVEL);

/* Module registry */
static sys_slist_t module_list = SYS_SLIST_STATIC_INIT(&module_list);
static K_MUTEX_DEFINE(module_mutex);
static bool module_system_initialized = false;

int akira_module_init(void)
{
	if (module_system_initialized) {
		LOG_WRN("Akira module system already initialized");
		return 0;
	}

	LOG_INF("Initializing Akira module system");
	module_system_initialized = true;

	return 0;
}

int akira_module_register(struct akira_module *module)
{
	if (!module || !module->name) {
		LOG_ERR("Invalid module descriptor");
		return -EINVAL;
	}

	k_mutex_lock(&module_mutex, K_FOREVER);

	/* Check if module already registered */
	sys_snode_t *node;
	SYS_SLIST_FOR_EACH_NODE(&module_list, node) {
		struct akira_module *existing = CONTAINER_OF(node, struct akira_module, node);
		if (strcmp(existing->name, module->name) == 0) {
			k_mutex_unlock(&module_mutex);
			LOG_ERR("Module '%s' already registered", module->name);
			return -EALREADY;
		}
	}

	/* Add to registry */
	sys_slist_append(&module_list, &module->node);
	k_mutex_unlock(&module_mutex);

	LOG_INF("Registered module: %s (type=%d)", module->name, module->type);

	/* Initialize module if callback provided */
	if (module->init) {
		int ret = module->init(module->user_data);
		if (ret < 0) {
			LOG_ERR("Module '%s' initialization failed: %d", module->name, ret);
			module->status = AKIRA_MODULE_STATUS_ERROR;
			return ret;
		}
	}

	module->status = AKIRA_MODULE_STATUS_INITIALIZED;
	return 0;
}

int akira_module_unregister(struct akira_module *module)
{
	if (!module) {
		return -EINVAL;
	}

	k_mutex_lock(&module_mutex, K_FOREVER);

	/* Deinitialize module */
	if (module->deinit) {
		module->deinit(module->user_data);
	}

	/* Remove from registry */
	sys_slist_find_and_remove(&module_list, &module->node);
	k_mutex_unlock(&module_mutex);

	module->status = AKIRA_MODULE_STATUS_UNINITIALIZED;
	LOG_INF("Unregistered module: %s", module->name);

	return 0;
}

struct akira_module *akira_module_find(const char *name)
{
	if (!name) {
		return NULL;
	}

	k_mutex_lock(&module_mutex, K_FOREVER);

	sys_snode_t *node;
	SYS_SLIST_FOR_EACH_NODE(&module_list, node) {
		struct akira_module *module = CONTAINER_OF(node, struct akira_module, node);
		if (strcmp(module->name, name) == 0) {
			k_mutex_unlock(&module_mutex);
			return module;
		}
	}

	k_mutex_unlock(&module_mutex);
	return NULL;
}

int akira_module_send_command(const char *module_name, const char *command,
                               void *data, size_t len)
{
	if (!module_name || !command) {
		return -EINVAL;
	}

	struct akira_module *module = akira_module_find(module_name);
	if (!module) {
		LOG_ERR("Module '%s' not found", module_name);
		return -ENOENT;
	}

	if (module->status != AKIRA_MODULE_STATUS_RUNNING &&
	    module->status != AKIRA_MODULE_STATUS_INITIALIZED) {
		LOG_WRN("Module '%s' not ready (status=%d)", module_name, module->status);
		return -EAGAIN;
	}

	if (!module->on_command) {
		LOG_WRN("Module '%s' has no command handler", module_name);
		return -ENOSYS;
	}

	LOG_DBG("Sending command '%s' to module '%s'", command, module_name);
	return module->on_command(command, data, len, module->user_data);
}

int akira_module_broadcast_event(const char *event, void *data, size_t len)
{
	if (!event) {
		return -EINVAL;
	}

	int count = 0;

	k_mutex_lock(&module_mutex, K_FOREVER);

	sys_snode_t *node;
	SYS_SLIST_FOR_EACH_NODE(&module_list, node) {
		struct akira_module *module = CONTAINER_OF(node, struct akira_module, node);
		
		if (module->on_event &&
		    (module->status == AKIRA_MODULE_STATUS_RUNNING ||
		     module->status == AKIRA_MODULE_STATUS_INITIALIZED)) {
			
			int ret = module->on_event(event, data, len, module->user_data);
			if (ret == 0) {
				count++;
			}
		}
	}

	k_mutex_unlock(&module_mutex);

	LOG_DBG("Broadcasted event '%s' to %d modules", event, count);
	return count;
}

enum akira_module_status akira_module_get_status(const char *module_name)
{
	struct akira_module *module = akira_module_find(module_name);
	if (!module) {
		return AKIRA_MODULE_STATUS_ERROR;
	}
	return module->status;
}
