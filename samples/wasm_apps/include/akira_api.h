/**
 * @file akira_api.h
 * @brief AkiraOS WASM API declarations
 * 
 * Include this header in your WASM apps to access AkiraOS native functions.
 * 
 * @note Not all APIs may be available depending on firmware configuration.
 *       Check the device's prj.conf for enabled features:
 *       - CONFIG_OCRE_TIMER - Timer APIs
 *       - CONFIG_OCRE_SENSORS - Sensor APIs
 *       - CONFIG_OCRE_GPIO - GPIO APIs
 *       - CONFIG_OCRE_CONTAINER_MESSAGING - Messaging APIs
 */

#ifndef AKIRA_API_H
#define AKIRA_API_H

#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Attribute Macros                                                          */
/*===========================================================================*/

/**
 * @brief Import a function from the host environment
 */
#define WASM_IMPORT(name) \
    __attribute__((import_module("env"))) \
    __attribute__((import_name(#name)))

/**
 * @brief Export a function to the host environment
 */
#define WASM_EXPORT(name) \
    __attribute__((export_name(#name)))

/*===========================================================================*/
/* OCRE Resource Types                                                       */
/*===========================================================================*/

/**
 * @brief Resource types for event dispatching
 */
typedef enum {
    OCRE_RESOURCE_TIMER    = 0,  /**< Timer resource */
    OCRE_RESOURCE_GPIO     = 1,  /**< GPIO resource */
    OCRE_RESOURCE_SENSOR   = 2,  /**< Sensor resource */
    OCRE_RESOURCE_MESSAGE  = 3,  /**< Messaging resource */
} ocre_resource_type_t;

/*===========================================================================*/
/* System Information                                                        */
/*===========================================================================*/

/**
 * @brief POSIX utsname-like structure for system info
 */
#define OCRE_API_POSIX_BUF_SIZE 65

typedef struct {
    char sysname[OCRE_API_POSIX_BUF_SIZE];   /**< OS name */
    char nodename[OCRE_API_POSIX_BUF_SIZE];  /**< Network node name */
    char release[OCRE_API_POSIX_BUF_SIZE];   /**< OS release */
    char version[OCRE_API_POSIX_BUF_SIZE];   /**< OS version */
    char machine[OCRE_API_POSIX_BUF_SIZE];   /**< Hardware type */
} ocre_utsname_t;

/*===========================================================================*/
/* Core APIs                                                                 */
/*===========================================================================*/

/**
 * @brief Sleep for specified milliseconds
 * @param milliseconds Duration to sleep
 * @return 0 on success
 */
WASM_IMPORT(ocre_sleep)
extern int ocre_sleep(int milliseconds);

/**
 * @brief Get system information
 * @param name Pointer to ocre_utsname_t structure
 * @return 0 on success, -1 on error
 */
WASM_IMPORT(uname)
extern int ocre_uname(ocre_utsname_t *name);

/*===========================================================================*/
/* Event/Dispatcher APIs                                                     */
/*===========================================================================*/

/**
 * @brief Register an event dispatcher function
 * 
 * Registers a WASM function to be called when events of the specified
 * resource type occur.
 * 
 * @param type Resource type (OCRE_RESOURCE_*)
 * @param function_name Name of the WASM function to call
 * @return 0 on success, negative on error
 */
WASM_IMPORT(ocre_register_dispatcher)
extern int ocre_register_dispatcher(int type, const char *function_name);

/**
 * @brief Wait for and retrieve an event
 * 
 * Blocks until an event occurs or timeout expires.
 * 
 * @param timeout_ms Timeout in milliseconds (-1 for infinite)
 * @param type Output: event type
 * @param id Output: resource ID (timer_id, pin_id, sensor_id, etc.)
 * @param param1 Output: type-specific parameter
 * @param param2 Output: type-specific parameter  
 * @param param3 Output: type-specific parameter
 * @return 0 on success, negative on error/timeout
 */
WASM_IMPORT(ocre_get_event)
extern int ocre_get_event(int timeout_ms, int *type, int *id,
                          int *param1, int *param2, int *param3);

/*===========================================================================*/
/* Sensor APIs (Requires CONFIG_OCRE_SENSORS)                                */
/*===========================================================================*/

/**
 * @brief Initialize the sensor subsystem
 * @return 0 on success, negative on error
 */
WASM_IMPORT(ocre_sensors_init)
extern int ocre_sensors_init(void);

/**
 * @brief Discover available sensors
 * @return Number of sensors found, negative on error
 */
WASM_IMPORT(ocre_sensors_discover)
extern int ocre_sensors_discover(void);

/**
 * @brief Open a sensor by index
 * @param index Sensor index (0 to discover count - 1)
 * @return 0 on success, negative on error
 */
WASM_IMPORT(ocre_sensors_open)
extern int ocre_sensors_open(int index);

/**
 * @brief Get sensor handle by index
 * @param index Sensor index
 * @return Sensor handle, or negative on error
 */
WASM_IMPORT(ocre_sensors_get_handle)
extern int ocre_sensors_get_handle(int index);

/**
 * @brief Get number of channels for a sensor
 * @param handle Sensor handle
 * @return Number of channels, or negative on error
 */
WASM_IMPORT(ocre_sensors_get_channel_count)
extern int ocre_sensors_get_channel_count(int handle);

/**
 * @brief Get channel type
 * @param handle Sensor handle
 * @param channel Channel index
 * @return Channel type, or negative on error
 */
WASM_IMPORT(ocre_sensors_get_channel_type)
extern int ocre_sensors_get_channel_type(int handle, int channel);

/**
 * @brief Read sensor value
 * @param handle Sensor handle
 * @param channel Channel index
 * @return Sensor reading as float
 */
WASM_IMPORT(ocre_sensors_read)
extern float ocre_sensors_read(int handle, int channel);

/**
 * @brief Open sensor by name
 * @param name Sensor name string
 * @return 0 on success, negative on error
 */
WASM_IMPORT(ocre_sensors_open_by_name)
extern int ocre_sensors_open_by_name(const char *name);

/**
 * @brief Get sensor handle by name
 * @param name Sensor name string
 * @return Sensor handle, or negative on error
 */
WASM_IMPORT(ocre_sensors_get_handle_by_name)
extern int ocre_sensors_get_handle_by_name(const char *name);

/*===========================================================================*/
/* GPIO APIs (Requires CONFIG_OCRE_GPIO)                                     */
/*===========================================================================*/

/** GPIO direction flags */
#define OCRE_GPIO_INPUT     (1 << 0)
#define OCRE_GPIO_OUTPUT    (1 << 1)
#define OCRE_GPIO_PULL_UP   (1 << 2)
#define OCRE_GPIO_PULL_DOWN (1 << 3)

/**
 * @brief Configure a GPIO pin
 * @param port GPIO port number
 * @param pin Pin number within port
 * @param flags Configuration flags (OCRE_GPIO_*)
 * @return 0 on success, negative on error
 */
WASM_IMPORT(ocre_gpio_configure)
extern int ocre_gpio_configure(int port, int pin, int flags);

/**
 * @brief Set GPIO output value
 * @param port GPIO port number
 * @param pin Pin number
 * @param value 0 for low, non-zero for high
 * @return 0 on success, negative on error
 */
WASM_IMPORT(ocre_gpio_set)
extern int ocre_gpio_set(int port, int pin, int value);

/**
 * @brief Get GPIO input value
 * @param port GPIO port number
 * @param pin Pin number
 * @return Pin value (0 or 1), negative on error
 */
WASM_IMPORT(ocre_gpio_get)
extern int ocre_gpio_get(int port, int pin);

/*===========================================================================*/
/* Messaging APIs (Requires CONFIG_OCRE_CONTAINER_MESSAGING)                 */
/*===========================================================================*/

/**
 * @brief Publish a message
 * @param topic Message topic string
 * @param content_type MIME type (e.g., "application/json")
 * @param payload Message payload data
 * @param payload_len Length of payload in bytes
 * @return 0 on success, negative on error
 */
WASM_IMPORT(ocre_publish_message)
extern int ocre_publish_message(const char *topic, const char *content_type,
                                void *payload, int payload_len);

/**
 * @brief Subscribe to messages on a topic
 * @param topic Topic pattern to subscribe to
 * @return 0 on success, negative on error
 */
WASM_IMPORT(ocre_subscribe_message)
extern int ocre_subscribe_message(const char *topic);

/**
 * @brief Free messaging event data
 * @param topic_offset Offset from event
 * @param content_type_offset Offset from event
 * @param payload_offset Offset from event
 * @return 0 on success
 */
WASM_IMPORT(ocre_messaging_free_module_event_data)
extern int ocre_messaging_free_module_event_data(int topic_offset,
                                                  int content_type_offset,
                                                  int payload_offset);

/*===========================================================================*/
/* Utility Macros                                                            */
/*===========================================================================*/

/**
 * @brief Define app entry point
 * 
 * Use this macro to define your app's main function:
 * @code
 * AKIRA_APP_MAIN() {
 *     // Your app code here
 *     while (1) {
 *         ocre_sleep(1000);
 *     }
 * }
 * @endcode
 */
#define AKIRA_APP_MAIN() \
    WASM_EXPORT(_start) void _start(void)

/**
 * @brief Define an event handler function
 * 
 * Use this macro to define event dispatcher callbacks:
 * @code
 * AKIRA_EVENT_HANDLER(on_timer_event) {
 *     // Handle timer event
 * }
 * @endcode
 */
#define AKIRA_EVENT_HANDLER(name) \
    WASM_EXPORT(name) void name(int type, int id, int p1, int p2, int p3)

#endif /* AKIRA_API_H */
