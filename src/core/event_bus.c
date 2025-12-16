/**
 * @file event_bus.c
 * @brief System Event Bus Implementation
 */

#include "event_bus.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(event_bus, CONFIG_AKIRA_LOG_LEVEL);

#define MAX_SUBSCRIBERS 32

typedef struct {
    event_type_t type;
    event_callback_t callback;
    void *user_data;
    bool active;
} subscriber_t;

static struct {
    bool initialized;
    subscriber_t subscribers[MAX_SUBSCRIBERS];
    struct k_mutex mutex;
} event_bus_state = {0};

int event_bus_init(void)
{
    if (event_bus_state.initialized) {
        return 0;
    }
    
    LOG_INF("Initializing event bus");
    
    k_mutex_init(&event_bus_state.mutex);
    memset(event_bus_state.subscribers, 0, sizeof(event_bus_state.subscribers));
    event_bus_state.initialized = true;
    
    LOG_INF("✅ Event bus initialized");
    return 0;
}

int event_bus_subscribe(event_type_t type, event_callback_t callback, void *user_data)
{
    if (!event_bus_state.initialized) {
        return -ENODEV;
    }
    
    if (!callback || type >= EVENT_MAX) {
        return -EINVAL;
    }
    
    k_mutex_lock(&event_bus_state.mutex, K_FOREVER);
    
    /* Check if already subscribed */
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (event_bus_state.subscribers[i].active &&
            event_bus_state.subscribers[i].type == type &&
            event_bus_state.subscribers[i].callback == callback) {
            k_mutex_unlock(&event_bus_state.mutex);
            LOG_WRN("Already subscribed to %s", event_type_to_string(type));
            return -EALREADY;
        }
    }
    
    /* Find free slot */
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (!event_bus_state.subscribers[i].active) {
            event_bus_state.subscribers[i].type = type;
            event_bus_state.subscribers[i].callback = callback;
            event_bus_state.subscribers[i].user_data = user_data;
            event_bus_state.subscribers[i].active = true;
            
            k_mutex_unlock(&event_bus_state.mutex);
            LOG_DBG("Subscribed to %s (slot %d)", event_type_to_string(type), i);
            return 0;
        }
    }
    
    k_mutex_unlock(&event_bus_state.mutex);
    LOG_ERR("No free subscriber slots");
    return -ENOMEM;
}

int event_bus_unsubscribe(event_type_t type, event_callback_t callback)
{
    if (!event_bus_state.initialized) {
        return -ENODEV;
    }
    
    if (!callback || type >= EVENT_MAX) {
        return -EINVAL;
    }
    
    k_mutex_lock(&event_bus_state.mutex, K_FOREVER);
    
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (event_bus_state.subscribers[i].active &&
            event_bus_state.subscribers[i].type == type &&
            event_bus_state.subscribers[i].callback == callback) {
            event_bus_state.subscribers[i].active = false;
            
            k_mutex_unlock(&event_bus_state.mutex);
            LOG_DBG("Unsubscribed from %s", event_type_to_string(type));
            return 0;
        }
    }
    
    k_mutex_unlock(&event_bus_state.mutex);
    return -ENOENT;
}

int event_bus_publish(const system_event_t *event)
{
    if (!event_bus_state.initialized) {
        return -ENODEV;
    }
    
    if (!event || event->type >= EVENT_MAX) {
        return -EINVAL;
    }
    
    LOG_DBG("Publishing event: %s", event_type_to_string(event->type));
    
    k_mutex_lock(&event_bus_state.mutex, K_FOREVER);
    
    int count = 0;
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (event_bus_state.subscribers[i].active &&
            event_bus_state.subscribers[i].type == event->type) {
            /* Call subscriber callback */
            event_bus_state.subscribers[i].callback(event, 
                event_bus_state.subscribers[i].user_data);
            count++;
        }
    }
    
    k_mutex_unlock(&event_bus_state.mutex);
    
    if (count > 0) {
        LOG_DBG("Event %s delivered to %d subscriber(s)", 
                event_type_to_string(event->type), count);
    }
    
    return 0;
}

const char *event_type_to_string(event_type_t type)
{
    switch (type) {
        case EVENT_SYSTEM_BOOT: return "SYSTEM_BOOT";
        case EVENT_SYSTEM_READY: return "SYSTEM_READY";
        case EVENT_SYSTEM_SHUTDOWN: return "SYSTEM_SHUTDOWN";
        case EVENT_SYSTEM_ERROR: return "SYSTEM_ERROR";
        
        case EVENT_NETWORK_CONNECTED: return "NETWORK_CONNECTED";
        case EVENT_NETWORK_DISCONNECTED: return "NETWORK_DISCONNECTED";
        case EVENT_NETWORK_IP_ASSIGNED: return "NETWORK_IP_ASSIGNED";
        case EVENT_NETWORK_ERROR: return "NETWORK_ERROR";
        
        case EVENT_BT_CONNECTED: return "BT_CONNECTED";
        case EVENT_BT_DISCONNECTED: return "BT_DISCONNECTED";
        case EVENT_BT_PAIRED: return "BT_PAIRED";
        case EVENT_BT_ADVERTISING: return "BT_ADVERTISING";
        
        case EVENT_STORAGE_READY: return "STORAGE_READY";
        case EVENT_STORAGE_MOUNTED: return "STORAGE_MOUNTED";
        case EVENT_STORAGE_UNMOUNTED: return "STORAGE_UNMOUNTED";
        case EVENT_STORAGE_ERROR: return "STORAGE_ERROR";
        
        case EVENT_OTA_STARTED: return "OTA_STARTED";
        case EVENT_OTA_PROGRESS: return "OTA_PROGRESS";
        case EVENT_OTA_COMPLETE: return "OTA_COMPLETE";
        case EVENT_OTA_ERROR: return "OTA_ERROR";
        
        case EVENT_APP_INSTALLED: return "APP_INSTALLED";
        case EVENT_APP_STARTED: return "APP_STARTED";
        case EVENT_APP_STOPPED: return "APP_STOPPED";
        case EVENT_APP_CRASHED: return "APP_CRASHED";
        
        case EVENT_SETTINGS_CHANGED: return "SETTINGS_CHANGED";
        case EVENT_SETTINGS_SAVED: return "SETTINGS_SAVED";
        
        case EVENT_BUTTON_PRESSED: return "BUTTON_PRESSED";
        case EVENT_BUTTON_RELEASED: return "BUTTON_RELEASED";
        case EVENT_DISPLAY_READY: return "DISPLAY_READY";
        
        case EVENT_POWER_LOW_BATTERY: return "POWER_LOW_BATTERY";
        case EVENT_POWER_CHARGING: return "POWER_CHARGING";
        case EVENT_POWER_SLEEP: return "POWER_SLEEP";
        
        default: return "UNKNOWN";
    }
}
