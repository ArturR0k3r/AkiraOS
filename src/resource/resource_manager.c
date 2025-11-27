/**
 * @file resource_manager.c
 * @brief System Resource Manager Implementation
 * 
 * Tracks and enforces resource quotas for WASM apps.
 * Prevents resource exhaustion attacks and ensures fair sharing.
 */

#include "resource_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(resource_mgr, CONFIG_AKIRA_LOG_LEVEL);

/* Warning threshold (percentage) */
#define QUOTA_WARNING_THRESHOLD     80

/* App resource tracking */
struct app_resources {
	bool in_use;
	uint32_t app_id;
	struct resource_quota quota;
	struct resource_usage usage;
	uint32_t last_cpu_sample;  // For CPU time tracking
};

/* Manager state */
static struct {
	bool initialized;
	struct app_resources apps[RESOURCE_MAX_APPS];
	struct resource_quota default_quota;
	struct k_mutex mutex;
	
	/* Event callbacks */
	resource_callback_t callbacks[4];
	void *callback_data[4];
	int callback_count;
	
	/* System totals */
	struct resource_usage system_usage;
} res_state;

/**
 * @brief Find app by ID
 */
static struct app_resources *find_app(uint32_t app_id)
{
	for (int i = 0; i < RESOURCE_MAX_APPS; i++) {
		if (res_state.apps[i].in_use && 
		    res_state.apps[i].app_id == app_id) {
			return &res_state.apps[i];
		}
	}
	return NULL;
}

/**
 * @brief Find free slot
 */
static struct app_resources *find_free_slot(void)
{
	for (int i = 0; i < RESOURCE_MAX_APPS; i++) {
		if (!res_state.apps[i].in_use) {
			return &res_state.apps[i];
		}
	}
	return NULL;
}

/**
 * @brief Notify callbacks of event
 */
static void notify_event(uint32_t app_id, resource_type_t type, 
                         resource_event_t event)
{
	for (int i = 0; i < res_state.callback_count; i++) {
		if (res_state.callbacks[i]) {
			res_state.callbacks[i](app_id, type, event, 
			                       res_state.callback_data[i]);
		}
	}
}

/**
 * @brief Get usage value by type
 */
static uint32_t get_usage_value(struct resource_usage *usage, resource_type_t type)
{
	switch (type) {
	case RESOURCE_MEMORY:     return usage->memory_bytes;
	case RESOURCE_CPU_TIME:   return usage->cpu_time_us;
	case RESOURCE_STORAGE:    return usage->storage_bytes;
	case RESOURCE_NETWORK_TX: return usage->network_tx_bytes;
	case RESOURCE_NETWORK_RX: return usage->network_rx_bytes;
	case RESOURCE_FILE_HANDLES: return usage->file_handles;
	case RESOURCE_SOCKETS:    return usage->sockets;
	default: return 0;
	}
}

/**
 * @brief Get quota value by type
 */
static uint32_t get_quota_value(struct resource_quota *quota, resource_type_t type)
{
	switch (type) {
	case RESOURCE_MEMORY:     return quota->memory_bytes;
	case RESOURCE_CPU_TIME:   return quota->cpu_time_us;
	case RESOURCE_STORAGE:    return quota->storage_bytes;
	case RESOURCE_NETWORK_TX: return quota->network_tx_bytes;
	case RESOURCE_NETWORK_RX: return quota->network_rx_bytes;
	case RESOURCE_FILE_HANDLES: return quota->file_handles;
	case RESOURCE_SOCKETS:    return quota->sockets;
	default: return 0;
	}
}

/**
 * @brief Set usage value by type
 */
static void set_usage_value(struct resource_usage *usage, resource_type_t type,
                            uint32_t value)
{
	switch (type) {
	case RESOURCE_MEMORY:     usage->memory_bytes = value; break;
	case RESOURCE_CPU_TIME:   usage->cpu_time_us = value; break;
	case RESOURCE_STORAGE:    usage->storage_bytes = value; break;
	case RESOURCE_NETWORK_TX: usage->network_tx_bytes = value; break;
	case RESOURCE_NETWORK_RX: usage->network_rx_bytes = value; break;
	case RESOURCE_FILE_HANDLES: usage->file_handles = value; break;
	case RESOURCE_SOCKETS:    usage->sockets = value; break;
	default: break;
	}
}

int resource_manager_init(void)
{
	if (res_state.initialized) {
		return 0;
	}
	
	LOG_INF("Initializing resource manager");
	
	k_mutex_init(&res_state.mutex);
	
	for (int i = 0; i < RESOURCE_MAX_APPS; i++) {
		res_state.apps[i].in_use = false;
	}
	
	/* Set sensible defaults */
	res_state.default_quota.memory_bytes = 64 * 1024;      // 64KB
	res_state.default_quota.cpu_time_us = 10 * 1000000;    // 10 seconds
	res_state.default_quota.storage_bytes = 128 * 1024;    // 128KB
	res_state.default_quota.network_tx_bytes = 1024 * 1024; // 1MB
	res_state.default_quota.network_rx_bytes = 1024 * 1024; // 1MB
	res_state.default_quota.file_handles = 8;
	res_state.default_quota.sockets = 4;
	
	memset(&res_state.system_usage, 0, sizeof(res_state.system_usage));
	
	res_state.initialized = true;
	
	LOG_INF("Resource manager initialized");
	return 0;
}

int resource_register_app(uint32_t app_id, const struct resource_quota *quota)
{
	if (!res_state.initialized) {
		return -ENODEV;
	}
	
	k_mutex_lock(&res_state.mutex, K_FOREVER);
	
	/* Check for duplicate */
	if (find_app(app_id)) {
		k_mutex_unlock(&res_state.mutex);
		LOG_WRN("App %u already registered", app_id);
		return -EEXIST;
	}
	
	struct app_resources *app = find_free_slot();
	if (!app) {
		k_mutex_unlock(&res_state.mutex);
		LOG_ERR("No free resource slots");
		return -ENOMEM;
	}
	
	app->in_use = true;
	app->app_id = app_id;
	app->quota = quota ? *quota : res_state.default_quota;
	memset(&app->usage, 0, sizeof(app->usage));
	app->last_cpu_sample = 0;
	
	k_mutex_unlock(&res_state.mutex);
	
	LOG_INF("Registered app %u (mem=%uKB, cpu=%ums)", 
	        app_id, app->quota.memory_bytes / 1024,
	        app->quota.cpu_time_us / 1000);
	
	return 0;
}

int resource_unregister_app(uint32_t app_id)
{
	if (!res_state.initialized) {
		return -ENODEV;
	}
	
	k_mutex_lock(&res_state.mutex, K_FOREVER);
	
	struct app_resources *app = find_app(app_id);
	if (!app) {
		k_mutex_unlock(&res_state.mutex);
		return -ENOENT;
	}
	
	app->in_use = false;
	
	k_mutex_unlock(&res_state.mutex);
	
	LOG_INF("Unregistered app %u", app_id);
	return 0;
}

void resource_set_default_quota(const struct resource_quota *quota)
{
	if (quota) {
		res_state.default_quota = *quota;
	}
}

int resource_update_quota(uint32_t app_id, const struct resource_quota *quota)
{
	if (!res_state.initialized || !quota) {
		return -EINVAL;
	}
	
	k_mutex_lock(&res_state.mutex, K_FOREVER);
	
	struct app_resources *app = find_app(app_id);
	if (!app) {
		k_mutex_unlock(&res_state.mutex);
		return -ENOENT;
	}
	
	app->quota = *quota;
	
	k_mutex_unlock(&res_state.mutex);
	
	LOG_INF("Updated quota for app %u", app_id);
	return 0;
}

int resource_request(uint32_t app_id, resource_type_t type, uint32_t amount)
{
	if (!res_state.initialized) {
		return -ENODEV;
	}
	
	if (type >= RESOURCE_TYPE_COUNT) {
		return -EINVAL;
	}
	
	k_mutex_lock(&res_state.mutex, K_FOREVER);
	
	struct app_resources *app = find_app(app_id);
	if (!app) {
		k_mutex_unlock(&res_state.mutex);
		return -ENOENT;
	}
	
	uint32_t current = get_usage_value(&app->usage, type);
	uint32_t quota = get_quota_value(&app->quota, type);
	uint32_t new_value = current + amount;
	
	/* Check quota */
	if (new_value > quota) {
		k_mutex_unlock(&res_state.mutex);
		LOG_WRN("App %u quota exceeded for resource %d", app_id, type);
		notify_event(app_id, type, RESOURCE_EVENT_QUOTA_EXCEEDED);
		return -EDQUOT;
	}
	
	/* Update usage */
	set_usage_value(&app->usage, type, new_value);
	
	/* Update system total */
	uint32_t sys_current = get_usage_value(&res_state.system_usage, type);
	set_usage_value(&res_state.system_usage, type, sys_current + amount);
	
	/* Check for warning threshold */
	if (quota > 0 && (new_value * 100 / quota) >= QUOTA_WARNING_THRESHOLD) {
		notify_event(app_id, type, RESOURCE_EVENT_QUOTA_WARNING);
	}
	
	k_mutex_unlock(&res_state.mutex);
	
	LOG_DBG("App %u allocated %u of resource %d (total: %u/%u)",
	        app_id, amount, type, new_value, quota);
	
	return 0;
}

int resource_release(uint32_t app_id, resource_type_t type, uint32_t amount)
{
	if (!res_state.initialized) {
		return -ENODEV;
	}
	
	if (type >= RESOURCE_TYPE_COUNT) {
		return -EINVAL;
	}
	
	k_mutex_lock(&res_state.mutex, K_FOREVER);
	
	struct app_resources *app = find_app(app_id);
	if (!app) {
		k_mutex_unlock(&res_state.mutex);
		return -ENOENT;
	}
	
	uint32_t current = get_usage_value(&app->usage, type);
	uint32_t new_value = (amount >= current) ? 0 : current - amount;
	
	set_usage_value(&app->usage, type, new_value);
	
	/* Update system total */
	uint32_t sys_current = get_usage_value(&res_state.system_usage, type);
	if (amount <= sys_current) {
		set_usage_value(&res_state.system_usage, type, sys_current - amount);
	}
	
	k_mutex_unlock(&res_state.mutex);
	
	LOG_DBG("App %u released %u of resource %d", app_id, amount, type);
	
	return 0;
}

int resource_get_usage(uint32_t app_id, struct resource_usage *usage)
{
	if (!res_state.initialized || !usage) {
		return -EINVAL;
	}
	
	k_mutex_lock(&res_state.mutex, K_FOREVER);
	
	struct app_resources *app = find_app(app_id);
	if (!app) {
		k_mutex_unlock(&res_state.mutex);
		return -ENOENT;
	}
	
	*usage = app->usage;
	
	k_mutex_unlock(&res_state.mutex);
	return 0;
}

int resource_get_quota(uint32_t app_id, struct resource_quota *quota)
{
	if (!res_state.initialized || !quota) {
		return -EINVAL;
	}
	
	k_mutex_lock(&res_state.mutex, K_FOREVER);
	
	struct app_resources *app = find_app(app_id);
	if (!app) {
		k_mutex_unlock(&res_state.mutex);
		return -ENOENT;
	}
	
	*quota = app->quota;
	
	k_mutex_unlock(&res_state.mutex);
	return 0;
}

bool resource_available(uint32_t app_id, resource_type_t type, uint32_t amount)
{
	if (!res_state.initialized || type >= RESOURCE_TYPE_COUNT) {
		return false;
	}
	
	k_mutex_lock(&res_state.mutex, K_FOREVER);
	
	struct app_resources *app = find_app(app_id);
	if (!app) {
		k_mutex_unlock(&res_state.mutex);
		return false;
	}
	
	uint32_t current = get_usage_value(&app->usage, type);
	uint32_t quota = get_quota_value(&app->quota, type);
	bool available = (current + amount) <= quota;
	
	k_mutex_unlock(&res_state.mutex);
	
	return available;
}

int resource_reset_usage(uint32_t app_id)
{
	if (!res_state.initialized) {
		return -ENODEV;
	}
	
	k_mutex_lock(&res_state.mutex, K_FOREVER);
	
	if (app_id == 0) {
		/* Reset all apps */
		for (int i = 0; i < RESOURCE_MAX_APPS; i++) {
			if (res_state.apps[i].in_use) {
				memset(&res_state.apps[i].usage, 0, 
				       sizeof(struct resource_usage));
				notify_event(res_state.apps[i].app_id, 0, 
				            RESOURCE_EVENT_RESET);
			}
		}
		memset(&res_state.system_usage, 0, sizeof(res_state.system_usage));
	} else {
		struct app_resources *app = find_app(app_id);
		if (!app) {
			k_mutex_unlock(&res_state.mutex);
			return -ENOENT;
		}
		memset(&app->usage, 0, sizeof(app->usage));
		notify_event(app_id, 0, RESOURCE_EVENT_RESET);
	}
	
	k_mutex_unlock(&res_state.mutex);
	
	LOG_INF("Reset resource usage for app %u", app_id);
	return 0;
}

int resource_register_callback(resource_callback_t callback, void *user_data)
{
	if (!callback || res_state.callback_count >= 4) {
		return -EINVAL;
	}
	
	res_state.callbacks[res_state.callback_count] = callback;
	res_state.callback_data[res_state.callback_count] = user_data;
	res_state.callback_count++;
	
	return 0;
}

void resource_get_system_usage(struct resource_usage *usage)
{
	if (usage) {
		k_mutex_lock(&res_state.mutex, K_FOREVER);
		*usage = res_state.system_usage;
		k_mutex_unlock(&res_state.mutex);
	}
}

void resource_print_report(void)
{
	LOG_INF("=== Resource Manager Report ===");
	
	k_mutex_lock(&res_state.mutex, K_FOREVER);
	
	LOG_INF("System totals:");
	LOG_INF("  Memory: %u bytes", res_state.system_usage.memory_bytes);
	LOG_INF("  CPU time: %u us", res_state.system_usage.cpu_time_us);
	LOG_INF("  Storage: %u bytes", res_state.system_usage.storage_bytes);
	
	LOG_INF("Per-app usage:");
	for (int i = 0; i < RESOURCE_MAX_APPS; i++) {
		if (res_state.apps[i].in_use) {
			struct app_resources *app = &res_state.apps[i];
			LOG_INF("  App %u:", app->app_id);
			LOG_INF("    Memory: %u/%u bytes",
			        app->usage.memory_bytes, app->quota.memory_bytes);
			LOG_INF("    CPU: %u/%u us",
			        app->usage.cpu_time_us, app->quota.cpu_time_us);
		}
	}
	
	k_mutex_unlock(&res_state.mutex);
}
