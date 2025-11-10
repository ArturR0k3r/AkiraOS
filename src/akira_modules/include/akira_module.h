/*
 * SPDX-License-Identifier: Apache-2.0
 * 
 * Akira Module System API
 * 
 * This API allows external projects to integrate Akira hardware
 * and control capabilities into their applications.
 */

#ifndef AKIRA_MODULE_H
#define AKIRA_MODULE_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Akira Module System
 * @defgroup akira_module Akira Module System
 * @{
 */

/** Module types */
enum akira_module_type {
	AKIRA_MODULE_TYPE_DISPLAY,      /**< Display control module */
	AKIRA_MODULE_TYPE_INPUT,        /**< Input/button control module */
	AKIRA_MODULE_TYPE_AUDIO,        /**< Audio control module */
	AKIRA_MODULE_TYPE_STORAGE,      /**< Storage access module */
	AKIRA_MODULE_TYPE_NETWORK,      /**< Network communication module */
	AKIRA_MODULE_TYPE_GPIO,         /**< GPIO control module */
	AKIRA_MODULE_TYPE_SENSOR,       /**< Sensor data module */
	AKIRA_MODULE_TYPE_CUSTOM,       /**< Custom user-defined module */
};

/** Module status */
enum akira_module_status {
	AKIRA_MODULE_STATUS_UNINITIALIZED,
	AKIRA_MODULE_STATUS_INITIALIZED,
	AKIRA_MODULE_STATUS_RUNNING,
	AKIRA_MODULE_STATUS_SUSPENDED,
	AKIRA_MODULE_STATUS_ERROR,
};

/** Communication interface types */
enum akira_comm_interface {
	AKIRA_COMM_UART,
	AKIRA_COMM_SPI,
	AKIRA_COMM_I2C,
	AKIRA_COMM_NETWORK,
	AKIRA_COMM_USB,
};

/** Module callback function types */
typedef int (*akira_module_init_fn)(void *user_data);
typedef int (*akira_module_deinit_fn)(void *user_data);
typedef int (*akira_module_command_fn)(const char *command, void *data, size_t len, void *user_data);
typedef int (*akira_module_event_fn)(const char *event, void *data, size_t len, void *user_data);

/** Module descriptor */
struct akira_module {
	const char *name;                       /**< Module name */
	enum akira_module_type type;            /**< Module type */
	enum akira_module_status status;        /**< Current status */
	
	/* Callbacks */
	akira_module_init_fn init;              /**< Init callback */
	akira_module_deinit_fn deinit;          /**< Deinit callback */
	akira_module_command_fn on_command;     /**< Command handler */
	akira_module_event_fn on_event;         /**< Event handler */
	
	void *user_data;                        /**< User data pointer */
	
	/* Internal use */
	sys_snode_t node;                       /**< Linked list node */
	uint32_t flags;                         /**< Module flags */
};

/** Module registration macro */
#define AKIRA_MODULE_DEFINE(_name, _type, _init, _deinit, _on_cmd, _on_evt, _data) \
	static struct akira_module _akira_module_##_name = { \
		.name = STRINGIFY(_name), \
		.type = _type, \
		.status = AKIRA_MODULE_STATUS_UNINITIALIZED, \
		.init = _init, \
		.deinit = _deinit, \
		.on_command = _on_cmd, \
		.on_event = _on_evt, \
		.user_data = _data, \
	}; \
	static int _akira_module_register_##_name(void) { \
		return akira_module_register(&_akira_module_##_name); \
	} \
	SYS_INIT(_akira_module_register_##_name, APPLICATION, 90)

/**
 * @brief Initialize the Akira module system
 * 
 * @return 0 on success, negative errno on failure
 */
int akira_module_init(void);

/**
 * @brief Register a module
 * 
 * @param module Pointer to module descriptor
 * @return 0 on success, negative errno on failure
 */
int akira_module_register(struct akira_module *module);

/**
 * @brief Unregister a module
 * 
 * @param module Pointer to module descriptor
 * @return 0 on success, negative errno on failure
 */
int akira_module_unregister(struct akira_module *module);

/**
 * @brief Find a module by name
 * 
 * @param name Module name
 * @return Pointer to module or NULL if not found
 */
struct akira_module *akira_module_find(const char *name);

/**
 * @brief Send a command to a module
 * 
 * @param module_name Target module name
 * @param command Command string
 * @param data Command data
 * @param len Data length
 * @return 0 on success, negative errno on failure
 */
int akira_module_send_command(const char *module_name, const char *command, 
                               void *data, size_t len);

/**
 * @brief Broadcast an event to all modules
 * 
 * @param event Event name
 * @param data Event data
 * @param len Data length
 * @return Number of modules that processed the event
 */
int akira_module_broadcast_event(const char *event, void *data, size_t len);

/**
 * @brief Get module status
 * 
 * @param module_name Module name
 * @return Module status or AKIRA_MODULE_STATUS_ERROR if not found
 */
enum akira_module_status akira_module_get_status(const char *module_name);

/**
 * @brief Set communication interface
 * 
 * @param interface Communication interface type
 * @param device Device to use for communication
 * @return 0 on success, negative errno on failure
 */
int akira_module_set_comm_interface(enum akira_comm_interface interface, 
                                     const struct device *device);

/**
 * @brief Start module system communication loop
 * 
 * @return 0 on success, negative errno on failure
 */
int akira_module_start_comm(void);

/**
 * @brief Stop module system communication loop
 * 
 * @return 0 on success, negative errno on failure
 */
int akira_module_stop_comm(void);

/**
 * @} end defgroup akira_module
 */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MODULE_H */
