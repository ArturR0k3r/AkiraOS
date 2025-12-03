/**
 * @file resource_manager.h
 * @brief System Resource Manager
 * 
 * Manages resource quotas and limits for WASM apps:
 * - Memory allocation limits
 * - CPU time quotas
 * - Storage quotas
 * - Network bandwidth limits
 */

#ifndef AKIRA_RESOURCE_MANAGER_H
#define AKIRA_RESOURCE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum tracked apps
 */
#define RESOURCE_MAX_APPS       16

/**
 * @brief Resource types
 */
typedef enum {
	RESOURCE_MEMORY,      // Heap memory in bytes
	RESOURCE_CPU_TIME,    // CPU time in microseconds
	RESOURCE_STORAGE,     // Storage in bytes
	RESOURCE_NETWORK_TX,  // Network TX bytes
	RESOURCE_NETWORK_RX,  // Network RX bytes
	RESOURCE_FILE_HANDLES,// Open file handles
	RESOURCE_SOCKETS,     // Open sockets
	RESOURCE_TYPE_COUNT
} resource_type_t;

/**
 * @brief Resource quota configuration
 */
struct resource_quota {
	uint32_t memory_bytes;
	uint32_t cpu_time_us;
	uint32_t storage_bytes;
	uint32_t network_tx_bytes;
	uint32_t network_rx_bytes;
	uint16_t file_handles;
	uint16_t sockets;
};

/**
 * @brief Resource usage snapshot
 */
struct resource_usage {
	uint32_t memory_bytes;
	uint32_t cpu_time_us;
	uint32_t storage_bytes;
	uint32_t network_tx_bytes;
	uint32_t network_rx_bytes;
	uint16_t file_handles;
	uint16_t sockets;
};

/**
 * @brief Resource event types
 */
typedef enum {
	RESOURCE_EVENT_QUOTA_WARNING,  // Approaching quota (80%)
	RESOURCE_EVENT_QUOTA_EXCEEDED, // Exceeded quota
	RESOURCE_EVENT_RESET           // Quota reset
} resource_event_t;

/**
 * @brief Resource event callback
 */
typedef void (*resource_callback_t)(uint32_t app_id, resource_type_t type,
                                    resource_event_t event, void *user_data);

/**
 * @brief Initialize resource manager
 * @return 0 on success
 */
int resource_manager_init(void);

/**
 * @brief Register app with resource quotas
 * @param app_id Application ID
 * @param quota Resource quotas
 * @return 0 on success
 */
int resource_register_app(uint32_t app_id, const struct resource_quota *quota);

/**
 * @brief Unregister app
 * @param app_id Application ID
 * @return 0 on success
 */
int resource_unregister_app(uint32_t app_id);

/**
 * @brief Set default quotas for new apps
 * @param quota Default quotas
 */
void resource_set_default_quota(const struct resource_quota *quota);

/**
 * @brief Update app quota
 * @param app_id Application ID
 * @param quota New quotas
 * @return 0 on success
 */
int resource_update_quota(uint32_t app_id, const struct resource_quota *quota);

/**
 * @brief Request resource allocation
 * @param app_id Application ID
 * @param type Resource type
 * @param amount Amount requested
 * @return 0 if allowed, -EDQUOT if would exceed quota
 */
int resource_request(uint32_t app_id, resource_type_t type, uint32_t amount);

/**
 * @brief Release resource
 * @param app_id Application ID
 * @param type Resource type
 * @param amount Amount released
 * @return 0 on success
 */
int resource_release(uint32_t app_id, resource_type_t type, uint32_t amount);

/**
 * @brief Get current usage
 * @param app_id Application ID
 * @param usage Output for usage data
 * @return 0 on success
 */
int resource_get_usage(uint32_t app_id, struct resource_usage *usage);

/**
 * @brief Get quota configuration
 * @param app_id Application ID
 * @param quota Output for quota data
 * @return 0 on success
 */
int resource_get_quota(uint32_t app_id, struct resource_quota *quota);

/**
 * @brief Check if resource is available
 * @param app_id Application ID
 * @param type Resource type
 * @param amount Amount needed
 * @return true if available
 */
bool resource_available(uint32_t app_id, resource_type_t type, uint32_t amount);

/**
 * @brief Reset usage counters (for periodic quotas)
 * @param app_id Application ID (0 for all)
 * @return 0 on success
 */
int resource_reset_usage(uint32_t app_id);

/**
 * @brief Register event callback
 * @param callback Callback function
 * @param user_data User context
 * @return 0 on success
 */
int resource_register_callback(resource_callback_t callback, void *user_data);

/**
 * @brief Get system-wide resource usage
 * @param usage Output for usage data
 */
void resource_get_system_usage(struct resource_usage *usage);

/**
 * @brief Print resource report (debug)
 */
void resource_print_report(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_RESOURCE_MANAGER_H */
