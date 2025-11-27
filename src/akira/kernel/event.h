/**
 * @file event.h
 * @brief AkiraOS Event System
 * 
 * Publish/subscribe event bus for inter-component communication.
 * Supports both synchronous and asynchronous event delivery.
 */

#ifndef AKIRA_KERNEL_EVENT_H
#define AKIRA_KERNEL_EVENT_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* Event Types                                                               */
/*===========================================================================*/

/**
 * @brief System event types
 */
typedef enum {
    /* System events (0-99) */
    AKIRA_EVENT_NONE = 0,
    AKIRA_EVENT_SYSTEM_READY,
    AKIRA_EVENT_SYSTEM_SHUTDOWN,
    AKIRA_EVENT_LOW_MEMORY,
    AKIRA_EVENT_LOW_BATTERY,
    
    /* Service events (100-199) */
    AKIRA_EVENT_SERVICE_STARTED = 100,
    AKIRA_EVENT_SERVICE_STOPPED,
    AKIRA_EVENT_SERVICE_ERROR,
    
    /* Process events (200-299) */
    AKIRA_EVENT_PROCESS_STARTED = 200,
    AKIRA_EVENT_PROCESS_STOPPED,
    AKIRA_EVENT_PROCESS_CRASHED,
    
    /* Input events (300-399) */
    AKIRA_EVENT_BUTTON_PRESS = 300,
    AKIRA_EVENT_BUTTON_RELEASE,
    AKIRA_EVENT_BUTTON_LONG_PRESS,
    AKIRA_EVENT_TOUCH_DOWN,
    AKIRA_EVENT_TOUCH_UP,
    AKIRA_EVENT_TOUCH_MOVE,
    
    /* Network events (400-499) */
    AKIRA_EVENT_WIFI_CONNECTED = 400,
    AKIRA_EVENT_WIFI_DISCONNECTED,
    AKIRA_EVENT_WIFI_SCAN_DONE,
    AKIRA_EVENT_BLE_CONNECTED,
    AKIRA_EVENT_BLE_DISCONNECTED,
    AKIRA_EVENT_RF_MESSAGE,
    
    /* Storage events (500-599) */
    AKIRA_EVENT_SD_INSERTED = 500,
    AKIRA_EVENT_SD_REMOVED,
    AKIRA_EVENT_FILE_CHANGED,
    
    /* OTA events (600-699) */
    AKIRA_EVENT_OTA_STARTED = 600,
    AKIRA_EVENT_OTA_PROGRESS,
    AKIRA_EVENT_OTA_COMPLETE,
    AKIRA_EVENT_OTA_FAILED,
    
    /* App events (700-799) */
    AKIRA_EVENT_APP_INSTALLED = 700,
    AKIRA_EVENT_APP_UNINSTALLED,
    AKIRA_EVENT_APP_STARTED,
    AKIRA_EVENT_APP_STOPPED,
    AKIRA_EVENT_WASM_LOADED,
    
    /* Display events (800-899) */
    AKIRA_EVENT_DISPLAY_ON = 800,
    AKIRA_EVENT_DISPLAY_OFF,
    AKIRA_EVENT_DISPLAY_BRIGHTNESS,
    
    /* Timer events (900-999) */
    AKIRA_EVENT_TIMER_EXPIRED = 900,
    
    /* Custom/user events (1000+) */
    AKIRA_EVENT_CUSTOM = 1000,
    
    AKIRA_EVENT_MAX = 0xFFFF
} akira_event_type_t;

/*===========================================================================*/
/* Event Priority                                                            */
/*===========================================================================*/

typedef enum {
    AKIRA_EVENT_PRIORITY_LOW = 0,
    AKIRA_EVENT_PRIORITY_NORMAL = 1,
    AKIRA_EVENT_PRIORITY_HIGH = 2,
    AKIRA_EVENT_PRIORITY_URGENT = 3
} akira_event_priority_t;

/*===========================================================================*/
/* Event Structure                                                           */
/*===========================================================================*/

/**
 * @brief Event data structure
 */
typedef struct akira_event {
    akira_event_type_t type;        /**< Event type */
    akira_event_priority_t priority; /**< Event priority */
    uint32_t timestamp;             /**< Event timestamp (ms) */
    uint32_t source_id;             /**< Source service/process ID */
    size_t data_size;               /**< Size of event data */
    void *data;                     /**< Event-specific data (may be NULL) */
} akira_event_t;

/*===========================================================================*/
/* Event Handler                                                             */
/*===========================================================================*/

/**
 * @brief Event handler callback
 * @param event Event data
 * @param user_data User context passed during subscription
 * @return 0 to continue propagation, non-zero to stop
 */
typedef int (*akira_event_handler_t)(const akira_event_t *event, void *user_data);

/*===========================================================================*/
/* Event System API                                                          */
/*===========================================================================*/

/**
 * @brief Initialize event system
 * @return 0 on success
 */
int akira_event_init(void);

/**
 * @brief Subscribe to event type
 * @param type Event type to subscribe to
 * @param handler Handler callback
 * @param user_data User context (passed to handler)
 * @return Subscription handle or negative error
 */
akira_subscription_t akira_event_subscribe(akira_event_type_t type,
                                           akira_event_handler_t handler,
                                           void *user_data);

/**
 * @brief Subscribe to range of events
 * @param type_min Minimum event type
 * @param type_max Maximum event type
 * @param handler Handler callback
 * @param user_data User context
 * @return Subscription handle or negative error
 */
akira_subscription_t akira_event_subscribe_range(akira_event_type_t type_min,
                                                 akira_event_type_t type_max,
                                                 akira_event_handler_t handler,
                                                 void *user_data);

/**
 * @brief Unsubscribe from events
 * @param subscription Subscription handle
 * @return 0 on success
 */
int akira_event_unsubscribe(akira_subscription_t subscription);

/**
 * @brief Publish event (synchronous)
 * 
 * Delivers event to all subscribers immediately in the calling context.
 * 
 * @param event Event to publish
 * @return 0 on success
 */
int akira_event_publish(const akira_event_t *event);

/**
 * @brief Queue event for async delivery
 * 
 * Queues event for delivery by the event processing thread.
 * 
 * @param event Event to queue
 * @return 0 on success, -1 if queue full
 */
int akira_event_post(const akira_event_t *event);

/**
 * @brief Publish simple event (no data)
 * @param type Event type
 * @return 0 on success
 */
int akira_event_emit(akira_event_type_t type);

/**
 * @brief Process queued events
 * 
 * Called from main loop to process async events.
 * 
 * @return Number of events processed
 */
int akira_event_process(void);

/**
 * @brief Wait for specific event
 * @param type Event type to wait for
 * @param event Output for received event
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, -1 on timeout
 */
int akira_event_wait(akira_event_type_t type, akira_event_t *event,
                     akira_duration_t timeout_ms);

/**
 * @brief Get event queue depth
 * @return Number of pending events
 */
int akira_event_pending_count(void);

/**
 * @brief Clear all pending events
 */
void akira_event_clear_queue(void);

/*===========================================================================*/
/* Convenience Macros                                                        */
/*===========================================================================*/

/**
 * @brief Create a simple event
 */
#define AKIRA_EVENT(type) \
    ((akira_event_t){ \
        .type = (type), \
        .priority = AKIRA_EVENT_PRIORITY_NORMAL, \
        .timestamp = k_uptime_get_32(), \
        .source_id = 0, \
        .data_size = 0, \
        .data = NULL \
    })

/**
 * @brief Create an event with data
 */
#define AKIRA_EVENT_WITH_DATA(type, ptr, size) \
    ((akira_event_t){ \
        .type = (type), \
        .priority = AKIRA_EVENT_PRIORITY_NORMAL, \
        .timestamp = k_uptime_get_32(), \
        .source_id = 0, \
        .data_size = (size), \
        .data = (ptr) \
    })

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_KERNEL_EVENT_H */
