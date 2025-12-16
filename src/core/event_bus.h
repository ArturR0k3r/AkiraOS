/**
 * @file event_bus.h
 * @brief System Event Bus for AkiraOS
 * 
 * Provides publish/subscribe event system for loose coupling between modules.
 * Modules can publish events and subscribe to events without knowing about each other.
 */

#ifndef AKIRA_EVENT_BUS_H
#define AKIRA_EVENT_BUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief System event types
 */
typedef enum {
    /* System Events (0-9) */
    EVENT_SYSTEM_BOOT = 0,
    EVENT_SYSTEM_READY,
    EVENT_SYSTEM_SHUTDOWN,
    EVENT_SYSTEM_ERROR,
    
    /* Network Events (10-19) */
    EVENT_NETWORK_CONNECTED = 10,
    EVENT_NETWORK_DISCONNECTED,
    EVENT_NETWORK_IP_ASSIGNED,
    EVENT_NETWORK_ERROR,
    
    /* Bluetooth Events (20-29) */
    EVENT_BT_CONNECTED = 20,
    EVENT_BT_DISCONNECTED,
    EVENT_BT_PAIRED,
    EVENT_BT_ADVERTISING,
    
    /* Storage Events (30-39) */
    EVENT_STORAGE_READY = 30,
    EVENT_STORAGE_MOUNTED,
    EVENT_STORAGE_UNMOUNTED,
    EVENT_STORAGE_ERROR,
    
    /* OTA Events (40-49) */
    EVENT_OTA_STARTED = 40,
    EVENT_OTA_PROGRESS,
    EVENT_OTA_COMPLETE,
    EVENT_OTA_ERROR,
    
    /* App Events (50-59) */
    EVENT_APP_INSTALLED = 50,
    EVENT_APP_STARTED,
    EVENT_APP_STOPPED,
    EVENT_APP_CRASHED,
    
    /* Settings Events (60-69) */
    EVENT_SETTINGS_CHANGED = 60,
    EVENT_SETTINGS_SAVED,
    
    /* Hardware Events (70-79) */
    EVENT_BUTTON_PRESSED = 70,
    EVENT_BUTTON_RELEASED,
    EVENT_DISPLAY_READY,
    
    /* Power Events (80-89) */
    EVENT_POWER_LOW_BATTERY = 80,
    EVENT_POWER_CHARGING,
    EVENT_POWER_SLEEP,
    
    EVENT_MAX
} event_type_t;

/**
 * @brief Network types
 */
typedef enum {
    NETWORK_TYPE_WIFI = 0,
    NETWORK_TYPE_BLUETOOTH,
    NETWORK_TYPE_USB,
    NETWORK_TYPE_ETHERNET
} network_type_t;

/**
 * @brief Event data union
 */
typedef struct {
    event_type_t type;
    uint64_t timestamp;
    
    union {
        /* Network event data */
        struct {
            network_type_t type;
            char ip_addr[16];
            bool connected;
        } network;
        
        /* OTA event data */
        struct {
            uint8_t percentage;
            size_t bytes_written;
            size_t total_size;
            const char *message;
        } ota;
        
        /* Settings event data */
        struct {
            const char *key;
            const void *value;
        } settings;
        
        /* App event data */
        struct {
            const char *name;
            int exit_code;
        } app;
        
        /* Button event data */
        struct {
            uint8_t button_id;
            uint32_t button_mask;
        } button;
        
        /* Storage event data */
        struct {
            const char *mount_point;
            int error_code;
        } storage;
        
        /* Generic data */
        struct {
            int code;
            const char *message;
        } generic;
    } data;
} system_event_t;

/**
 * @brief Event callback function type
 * 
 * @param event The system event
 * @param user_data User-provided context data
 */
typedef void (*event_callback_t)(const system_event_t *event, void *user_data);

/**
 * @brief Initialize the event bus
 * 
 * @return 0 on success, negative errno on failure
 */
int event_bus_init(void);

/**
 * @brief Subscribe to an event type
 * 
 * @param type Event type to subscribe to
 * @param callback Callback function to invoke
 * @param user_data User context data passed to callback
 * @return 0 on success, negative errno on failure
 */
int event_bus_subscribe(event_type_t type, event_callback_t callback, void *user_data);

/**
 * @brief Unsubscribe from an event type
 * 
 * @param type Event type to unsubscribe from
 * @param callback Callback function to remove
 * @return 0 on success, negative errno on failure
 */
int event_bus_unsubscribe(event_type_t type, event_callback_t callback);

/**
 * @brief Publish an event to all subscribers
 * 
 * @param event Event to publish
 * @return 0 on success, negative errno on failure
 */
int event_bus_publish(const system_event_t *event);

/**
 * @brief Get the name of an event type (for logging)
 * 
 * @param type Event type
 * @return String name of the event
 */
const char *event_type_to_string(event_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_EVENT_BUS_H */
