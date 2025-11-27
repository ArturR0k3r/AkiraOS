/**
 * @file usb_manager.c
 * @brief USB Manager Implementation for AkiraOS
 */

#include "usb_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#if defined(CONFIG_USB_DEVICE_STACK)
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#define USB_AVAILABLE 1
#else
#define USB_AVAILABLE 0
#endif

LOG_MODULE_REGISTER(usb_manager, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct {
    bool initialized;
    usb_config_t config;
    usb_state_t state;
    usb_stats_t stats;
    
    usb_event_callback_t event_cb;
    void *event_cb_data;
    
    struct k_mutex mutex;
} usb_mgr;

/*===========================================================================*/
/* Internal Functions                                                        */
/*===========================================================================*/

static void notify_event(usb_event_t event)
{
    if (usb_mgr.event_cb) {
        usb_mgr.event_cb(event, usb_mgr.event_cb_data);
    }
}

#if USB_AVAILABLE
static void usb_status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
    switch (status) {
    case USB_DC_CONNECTED:
        LOG_INF("USB connected");
        usb_mgr.state = USB_STATE_CONNECTED;
        usb_mgr.stats.vbus_present = true;
        notify_event(USB_EVENT_CONNECTED);
        break;
        
    case USB_DC_DISCONNECTED:
        LOG_INF("USB disconnected");
        usb_mgr.state = USB_STATE_READY;
        usb_mgr.stats.vbus_present = false;
        notify_event(USB_EVENT_DISCONNECTED);
        break;
        
    case USB_DC_CONFIGURED:
        LOG_INF("USB configured");
        usb_mgr.state = USB_STATE_CONNECTED;
        break;
        
    case USB_DC_SUSPEND:
        LOG_INF("USB suspended");
        usb_mgr.state = USB_STATE_SUSPENDED;
        notify_event(USB_EVENT_SUSPENDED);
        break;
        
    case USB_DC_RESUME:
        LOG_INF("USB resumed");
        usb_mgr.state = USB_STATE_CONNECTED;
        notify_event(USB_EVENT_RESUMED);
        break;
        
    default:
        break;
    }
}
#endif

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int usb_manager_init(const usb_config_t *config)
{
    if (usb_mgr.initialized) {
        return 0;
    }
    
    LOG_INF("Initializing USB manager");
    
    k_mutex_init(&usb_mgr.mutex);
    memset(&usb_mgr.stats, 0, sizeof(usb_stats_t));
    
    if (config) {
        memcpy(&usb_mgr.config, config, sizeof(usb_config_t));
    } else {
        usb_mgr.config.manufacturer = "AkiraOS";
        usb_mgr.config.product = "AkiraOS Device";
        usb_mgr.config.serial = "0001";
        usb_mgr.config.vendor_id = 0x1234;
        usb_mgr.config.product_id = 0x5678;
        usb_mgr.config.classes = USB_CLASS_HID;
    }
    
    usb_mgr.state = USB_STATE_INITIALIZING;
    
#if USB_AVAILABLE
    int ret = usb_enable(usb_status_cb);
    if (ret != 0) {
        LOG_ERR("USB enable failed: %d", ret);
        usb_mgr.state = USB_STATE_ERROR;
        return ret;
    }
    
    usb_mgr.state = USB_STATE_READY;
    usb_mgr.initialized = true;
    
    LOG_INF("USB initialized: %s", usb_mgr.config.product);
    return 0;
#else
    LOG_WRN("USB not available (simulation mode)");
    usb_mgr.state = USB_STATE_READY;
    usb_mgr.initialized = true;
    return 0;
#endif
}

int usb_manager_deinit(void)
{
    if (!usb_mgr.initialized) {
        return 0;
    }
    
#if USB_AVAILABLE
    usb_disable();
#endif
    
    usb_mgr.initialized = false;
    usb_mgr.state = USB_STATE_OFF;
    
    LOG_INF("USB manager deinitialized");
    return 0;
}

int usb_manager_enable(void)
{
    if (!usb_mgr.initialized) {
        return -EINVAL;
    }
    
#if USB_AVAILABLE
    return usb_enable(usb_status_cb);
#else
    usb_mgr.state = USB_STATE_READY;
    return 0;
#endif
}

int usb_manager_disable(void)
{
#if USB_AVAILABLE
    usb_disable();
#endif
    usb_mgr.state = USB_STATE_OFF;
    return 0;
}

usb_state_t usb_manager_get_state(void)
{
    return usb_mgr.state;
}

int usb_manager_get_stats(usb_stats_t *stats)
{
    if (!stats) {
        return -EINVAL;
    }
    
    k_mutex_lock(&usb_mgr.mutex, K_FOREVER);
    memcpy(stats, &usb_mgr.stats, sizeof(usb_stats_t));
    stats->state = usb_mgr.state;
    k_mutex_unlock(&usb_mgr.mutex);
    
    return 0;
}

bool usb_manager_is_connected(void)
{
    return usb_mgr.state == USB_STATE_CONNECTED;
}

int usb_manager_register_callback(usb_event_callback_t callback, void *user_data)
{
    usb_mgr.event_cb = callback;
    usb_mgr.event_cb_data = user_data;
    return 0;
}
