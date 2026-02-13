/**
 * @file usb_manager.c
 * @brief USB Device Manager Implementation for Zephyr RTOS 4.3
 */

#include "usb_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(usb_manager, CONFIG_LOG_DEFAULT_LEVEL);

/**
 * @brief Callback entry structure
 */
struct callback_entry {
    usb_manager_event_cb_t callback;
    void *user_data;
    bool active;
};

/**
 * @brief USB manager context structure
 */
struct usb_manager_context {
    struct usbd_context *usbd_ctx;
    usb_manager_state_t state;
    struct k_mutex mutex;
    bool initialized;
    struct callback_entry callbacks[USB_MANAGER_MAX_CALLBACKS];
    usb_manager_stats_t stats;
};

/* Global USB manager context */
static struct usb_manager_context usb_mgr_ctx = {
    .state = USB_STATE_DISABLED,
    .initialized = false,
};

/* USB device context */
USBD_DEVICE_DEFINE(device_usbd,
                   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
                   0x2fe3, 0x0001);

/**
 * @brief Notify all registered callbacks of an event
 * 
 * @param event The event to notify
 */
static void usb_manager_notify_callbacks(usb_manager_event_t event)
{
    
    for (int i = 0; i < USB_MANAGER_MAX_CALLBACKS; i++) {
        if (usb_mgr_ctx.callbacks[i].active && usb_mgr_ctx.callbacks[i].callback) {
            usb_mgr_ctx.callbacks[i].callback(event, usb_mgr_ctx.callbacks[i].user_data);
        }
    }
}

/**
 * @brief Update USB manager state
 * 
 * @param new_state The new state to transition to
 */
static void usb_manager_set_state(usb_manager_state_t new_state)
{
    usb_manager_state_t old_state = usb_mgr_ctx.state;
    
    if (old_state != new_state) {
        LOG_INF("State transition: %s -> %s",
                usb_manager_state_to_string(old_state),
                usb_manager_state_to_string(new_state));
        usb_mgr_ctx.state = new_state;
    }
}

/**
 * @brief USB device message callback
 * 
 * This is the main callback that handles all USB device events
 */
static void usb_manager_msg_cb(struct usbd_context *const ctx,
                                const struct usbd_msg *const msg)
{
    LOG_INF("USB message: %s (status=%d)", 
            usbd_msg_type_string(msg->type), msg->status);
    
    k_mutex_lock(&usb_mgr_ctx.mutex, K_FOREVER);
    
    /* Handle different message types */
    switch (msg->type) {
    case USBD_MSG_CONFIGURATION:
        LOG_INF("USB device configured (config=%d)", msg->status);
        usb_mgr_ctx.stats.configured_count++;
        usb_manager_set_state(USB_STATE_CONFIGURED);
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        usb_manager_notify_callbacks(USB_EVENT_CONFIGURED);
        break;
        
    case USBD_MSG_SUSPEND:
        if (!usbd_can_detect_vbus(usb_mgr_ctx.usbd_ctx)) {
            k_mutex_unlock(&usb_mgr_ctx.mutex);
            break;
        }
        LOG_INF("USB device suspended");
        usb_mgr_ctx.stats.suspended_count++;
        usb_manager_set_state(USB_STATE_SUSPENDED);
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        usb_manager_notify_callbacks(USB_EVENT_SUSPENDED);
        break;
        
    case USBD_MSG_RESUME:
        LOG_INF("USB device resumed");
        usb_mgr_ctx.stats.resumed_count++;
        usb_manager_set_state(USB_STATE_CONFIGURED);
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        usb_manager_notify_callbacks(USB_EVENT_RESUMED);
        break;
        
    case USBD_MSG_RESET:
        LOG_INF("USB device reset");
        usb_mgr_ctx.stats.reset_count++;
        usb_manager_set_state(USB_STATE_ENABLED);
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        usb_manager_notify_callbacks(USB_EVENT_RESET);
        break;
        
    case USBD_MSG_VBUS_READY:
        if (!usbd_can_detect_vbus(usb_mgr_ctx.usbd_ctx)) {
            k_mutex_unlock(&usb_mgr_ctx.mutex);
            break;
        }
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        break;
        
    case USBD_MSG_VBUS_REMOVED:
        if (!usbd_can_detect_vbus(usb_mgr_ctx.usbd_ctx)) {
            k_mutex_unlock(&usb_mgr_ctx.mutex);
            break;
        }
        LOG_INF("VBUS removed");
        usb_manager_set_state(USB_STATE_INITIALIZED);
        usb_mgr_ctx.stats.error_count++;
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        usb_manager_notify_callbacks(USB_EVENT_DISCONNECTED);
        break;
        
    case USBD_MSG_UDC_ERROR:
        LOG_ERR("UDC error");
        usb_manager_set_state(USB_STATE_ERROR);
        usb_mgr_ctx.stats.error_count++;
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        usb_manager_notify_callbacks(USB_EVENT_ERROR);
        break;
        
    default:
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        LOG_INF("Unhandled USB message type: %d", msg->type);
        break;
    }
}

USBD_DESC_LANG_DEFINE(device_lang); 
USBD_DESC_MANUFACTURER_DEFINE(device_mfr, "AkiraOS");
USBD_DESC_PRODUCT_DEFINE(device_product, "Akira USB Device");

static const uint8_t attributes = USB_SCD_SELF_POWERED | USB_SCD_REMOTE_WAKEUP;

USBD_CONFIGURATION_DEFINE(fs_cfg_desc,
                          attributes,
                          100, // 200 mA 
                          &fs_cfg_desc);

int usb_manager_init(void)
{
    int ret;
    
    if (usb_mgr_ctx.initialized) {
        LOG_ERR("USB manager already initialized");
        return -EALREADY;
    }
    
    LOG_INF("Initializing USB manager");
    
    /* Initialize mutex */
    ret = k_mutex_init(&usb_mgr_ctx.mutex);
    if (ret != 0) {
        LOG_ERR("Failed to initialize mutex: %d", ret);
        return ret;
    }
    
    k_mutex_lock(&usb_mgr_ctx.mutex, K_FOREVER);
    
    /* Clear callbacks */
    memset(usb_mgr_ctx.callbacks, 0, sizeof(usb_mgr_ctx.callbacks));
    
    /* Clear statistics */
    memset(&usb_mgr_ctx.stats, 0, sizeof(usb_mgr_ctx.stats));
    
    /* Get USB device context */
    usb_mgr_ctx.usbd_ctx = &device_usbd;
    if (usb_mgr_ctx.usbd_ctx == NULL) {
        LOG_ERR("USB device context not available");
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return -ENODEV;
    }
    LOG_INF("USB device context obtained");
    
    ret = usbd_add_descriptor(usb_mgr_ctx.usbd_ctx, &device_lang);
    if(ret){
        LOG_ERR("Failed to add language descriptor: %d", ret);
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return ret;
    }

    ret = usbd_add_descriptor(usb_mgr_ctx.usbd_ctx, &device_mfr);
    if(ret){
        LOG_ERR("Failed to add manufacturer descriptor: %d", ret);
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return ret;
    }
    
    ret = usbd_add_descriptor(usb_mgr_ctx.usbd_ctx, &device_product);
    if(ret){
        LOG_ERR("Failed to add product descriptor: %d", ret);
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return ret;
    }

    ret = usbd_add_configuration(usb_mgr_ctx.usbd_ctx, USBD_SPEED_FS, &fs_cfg_desc);
    if(ret){
        LOG_ERR("Failed to add configuration descriptor: %d", ret);
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return ret;
    }

    /* Register message callback */
    ret = usbd_msg_register_cb(usb_mgr_ctx.usbd_ctx, usb_manager_msg_cb);
    if (ret != 0) {
        LOG_ERR("Failed to register USB message callback: %d", ret);
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return ret;
    }
    LOG_INF("Message callback registered");
    
    /*
    usbd_init() will be called in usb_manager_finalize since we need to add
    more characteristics and classes before initializing the USB device stack, 
    depending on the use of the USB device (HID, MASS, etc.)
    */
    
    usb_mgr_ctx.initialized = true;
    usb_manager_set_state(USB_STATE_INITIALIZED);
    
    k_mutex_unlock(&usb_mgr_ctx.mutex);
    
    LOG_INF("USB manager initialized (waiting for class registration)");
    return 0;
}

int usb_manager_finalize(void)
{
    int ret;
    
    if (!usb_mgr_ctx.initialized) {
        LOG_ERR("USB manager not initialized");
        return -EINVAL;
    }
    
    LOG_INF("Finalizing USB device stack");
    
    k_mutex_lock(&usb_mgr_ctx.mutex, K_FOREVER);
    
    /* Initialize USB device stack (after classes registered) */
    ret = usbd_init(usb_mgr_ctx.usbd_ctx);
    if (ret != 0) {
        LOG_ERR("Failed to initialize USB device stack: %d", ret);
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return ret;
    }
    LOG_INF("USB device stack initialized");
    
    k_mutex_unlock(&usb_mgr_ctx.mutex);
    
    LOG_INF("USB manager finalized successfully");
    return 0;
}

int usb_manager_deinit(void)
{
    int ret;
    
    if (!usb_mgr_ctx.initialized) {
        LOG_ERR("USB manager not initialized");
        return -EINVAL;
    }
    
    LOG_INF("Deinitializing USB manager");
    
    k_mutex_lock(&usb_mgr_ctx.mutex, K_FOREVER);
    
    /* Disable USB if it's enabled */
    if (usb_mgr_ctx.state != USB_STATE_DISABLED && 
        usb_mgr_ctx.state != USB_STATE_INITIALIZED) {
        ret = usbd_disable(usb_mgr_ctx.usbd_ctx);
        if (ret != 0) {
            LOG_WRN("Failed to disable USB during deinit: %d", ret);
        }
    }
    
    /* Shutdown USB device stack */
    ret = usbd_shutdown(usb_mgr_ctx.usbd_ctx);
    if (ret != 0) {
        LOG_ERR("Failed to shutdown USB device stack: %d", ret);
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return ret;
    }
    
    /* Clear callbacks */
    memset(usb_mgr_ctx.callbacks, 0, sizeof(usb_mgr_ctx.callbacks));
    
    usb_mgr_ctx.initialized = false;
    usb_manager_set_state(USB_STATE_DISABLED);
    
    k_mutex_unlock(&usb_mgr_ctx.mutex);
    
    LOG_INF("USB manager deinitialized successfully");
    return 0;
}

int usb_manager_enable(void)
{
    int ret;
    
    if (!usb_mgr_ctx.initialized) {
        LOG_ERR("USB manager not initialized");
        return -EINVAL;
    }
    
    k_mutex_lock(&usb_mgr_ctx.mutex, K_FOREVER);
    
    if (usb_mgr_ctx.state != USB_STATE_INITIALIZED && 
        usb_mgr_ctx.state != USB_STATE_DISABLED) {
        LOG_ERR("USB already enabled (state: %s)", 
                usb_manager_state_to_string(usb_mgr_ctx.state));
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return -EALREADY;
    }
    
    LOG_INF("Enabling USB device");
    
    ret = usbd_enable(usb_mgr_ctx.usbd_ctx);
    if (ret != 0) {
        LOG_ERR("Failed to enable USB device: %d", ret);
        usb_manager_set_state(USB_STATE_ERROR);
        usb_mgr_ctx.stats.error_count++;
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return -EIO;
    }
    
    usb_manager_set_state(USB_STATE_ENABLED);
    
    k_mutex_unlock(&usb_mgr_ctx.mutex);
    
    LOG_INF("USB device enabled successfully");
    return 0;
}

int usb_manager_disable(void)
{
    int ret;
    
    if (!usb_mgr_ctx.initialized) {
        LOG_ERR("USB manager not initialized");
        return -EINVAL;
    }
    
    k_mutex_lock(&usb_mgr_ctx.mutex, K_FOREVER);
    
    if (usb_mgr_ctx.state == USB_STATE_DISABLED || 
        usb_mgr_ctx.state == USB_STATE_INITIALIZED) {
        LOG_ERR("USB not enabled (state: %s)", 
                usb_manager_state_to_string(usb_mgr_ctx.state));
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return -EINVAL;
    }
    
    LOG_INF("Disabling USB device");
    
    ret = usbd_disable(usb_mgr_ctx.usbd_ctx);
    if (ret != 0) {
        LOG_ERR("Failed to disable USB device: %d", ret);
        usb_manager_set_state(USB_STATE_ERROR);
        usb_mgr_ctx.stats.error_count++;
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return -EIO;
    }
    
    usb_manager_set_state(USB_STATE_INITIALIZED);
    usb_manager_notify_callbacks(USB_EVENT_DISCONNECTED);
    
    k_mutex_unlock(&usb_mgr_ctx.mutex);
    
    LOG_INF("USB device disabled successfully");
    return 0;
}

int usb_manager_register_callback(usb_manager_event_cb_t callback, void *user_data)
{
    int handle = -1;
    
    if (!usb_mgr_ctx.initialized) {
        LOG_ERR("USB manager not initialized");
        return -EINVAL;
    }
    
    if (callback == NULL) {
        LOG_ERR("Invalid callback pointer");
        return -EINVAL;
    }
    
    k_mutex_lock(&usb_mgr_ctx.mutex, K_FOREVER);
    
    /* Find an empty slot */
    for (int i = 0; i < USB_MANAGER_MAX_CALLBACKS; i++) {
        if (!usb_mgr_ctx.callbacks[i].active) {
            usb_mgr_ctx.callbacks[i].callback = callback;
            usb_mgr_ctx.callbacks[i].user_data = user_data;
            usb_mgr_ctx.callbacks[i].active = true;
            handle = i;
            break;
        }
    }
    
    k_mutex_unlock(&usb_mgr_ctx.mutex);
    
    if (handle < 0) {
        LOG_ERR("Maximum number of callbacks (%d) already registered", 
                USB_MANAGER_MAX_CALLBACKS);
        return -ENOMEM;
    }
    
    LOG_INF("Registered callback at handle %d", handle);
    return handle;
}

int usb_manager_unregister_callback(int callback_handle)
{
    if (!usb_mgr_ctx.initialized) {
        LOG_ERR("USB manager not initialized");
        return -EINVAL;
    }
    
    if (callback_handle < 0 || callback_handle >= USB_MANAGER_MAX_CALLBACKS) {
        LOG_ERR("Invalid callback handle: %d", callback_handle);
        return -EINVAL;
    }
    
    k_mutex_lock(&usb_mgr_ctx.mutex, K_FOREVER);
    
    if (!usb_mgr_ctx.callbacks[callback_handle].active) {
        LOG_ERR("Callback handle %d not active", callback_handle);
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return -EINVAL;
    }
    
    usb_mgr_ctx.callbacks[callback_handle].active = false;
    usb_mgr_ctx.callbacks[callback_handle].callback = NULL;
    usb_mgr_ctx.callbacks[callback_handle].user_data = NULL;
    
    k_mutex_unlock(&usb_mgr_ctx.mutex);
    
    LOG_INF("Unregistered callback handle %d", callback_handle);
    return 0;
}

const char *usb_manager_state_to_string(usb_manager_state_t state)
{
    switch (state) {
    case USB_STATE_DISABLED:
        return "DISABLED";
    case USB_STATE_INITIALIZED:
        return "INITIALIZED";
    case USB_STATE_ENABLED:
        return "ENABLED";
    case USB_STATE_CONFIGURED:
        return "CONFIGURED";
    case USB_STATE_SUSPENDED:
        return "SUSPENDED";
    case USB_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

const char *usb_manager_event_to_string(usb_manager_event_t event)
{
    switch (event) {
    case USB_EVENT_CONFIGURED:
        return "CONFIGURED";
    case USB_EVENT_SUSPENDED:
        return "SUSPENDED";
    case USB_EVENT_RESUMED:
        return "RESUMED";
    case USB_EVENT_RESET:
        return "RESET";
    case USB_EVENT_DISCONNECTED:
        return "DISCONNECTED";
    case USB_EVENT_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

bool usb_manager_is_configured(void)
{
    return usb_mgr_ctx.state == USB_STATE_CONFIGURED;
}

bool usb_manager_is_enabled(void)
{
    return usb_mgr_ctx.state != USB_STATE_DISABLED && usb_mgr_ctx.state != USB_STATE_INITIALIZED;
}

int usb_manager_get_stats(usb_manager_stats_t *stats)
{
    if (!usb_mgr_ctx.initialized) {
        LOG_ERR("USB manager not initialized");
        return -EINVAL;
    }
    
    if (stats == NULL) {
        LOG_ERR("Invalid stats pointer");
        return -EINVAL;
    }
    
    k_mutex_lock(&usb_mgr_ctx.mutex, K_FOREVER);
    memcpy(stats, &usb_mgr_ctx.stats, sizeof(usb_manager_stats_t));
    k_mutex_unlock(&usb_mgr_ctx.mutex);
    
    return 0;
}

int usb_manager_reset_stats(void)
{
    if (!usb_mgr_ctx.initialized) {
        LOG_ERR("USB manager not initialized");
        return -EINVAL;
    }
    
    k_mutex_lock(&usb_mgr_ctx.mutex, K_FOREVER);
    memset(&usb_mgr_ctx.stats, 0, sizeof(usb_manager_stats_t));
    k_mutex_unlock(&usb_mgr_ctx.mutex);
    
    LOG_INF("USB manager statistics reset");
    return 0;
}

struct usbd_context *usb_manager_get_context(void)
{
    if (!usb_mgr_ctx.initialized) {
        return NULL;
    }
    
    return usb_mgr_ctx.usbd_ctx;
}

int usb_manager_remote_wakeup(void)
{
    int ret;
    
    if (!usb_mgr_ctx.initialized) {
        LOG_ERR("USB manager not initialized");
        return -EINVAL;
    }
    
    k_mutex_lock(&usb_mgr_ctx.mutex, K_FOREVER);
    
    if (usb_mgr_ctx.state != USB_STATE_SUSPENDED) {
        LOG_ERR("Device not in suspended state (state: %s)", 
                usb_manager_state_to_string(usb_mgr_ctx.state));
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return -EAGAIN;
    }
    
    LOG_INF("Triggering remote wakeup");
    
    ret = usbd_wakeup_request(usb_mgr_ctx.usbd_ctx);
    if (ret != 0) {
        LOG_ERR("Failed to request remote wakeup: %d", ret);
        k_mutex_unlock(&usb_mgr_ctx.mutex);
        return ret;
    }
    
    k_mutex_unlock(&usb_mgr_ctx.mutex);
    
    return 0;
}

int usb_manager_is_initialized(void){
    return usb_mgr_ctx.initialized;
}

usb_manager_state_t usb_manager_get_state(void)
{
    return usb_mgr_ctx.state;
}
