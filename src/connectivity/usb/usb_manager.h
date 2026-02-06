/**
 * @file usb_manager.h
 * @brief USB Device Manager for Zephyr RTOS 4.3 (USB Device Stack Next)
 * 
 * This module provides a thread-safe USB device management layer with:
 * - Initialization and deinitialization
 * - Enable/disable functionality
 * - State management and tracking
 * - Callback registration for USB events
 * - Support for multiple USB classes (HID, Mass Storage, etc.)
 */

#ifndef USB_MANAGER_H_
#define USB_MANAGER_H_

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief USB device state enum
 */
typedef enum {
    USB_STATE_DISABLED,      /**< USB is disabled */
    USB_STATE_INITIALIZED,   /**< USB is initialized but not enabled */
    USB_STATE_ENABLED,       /**< USB is enabled but not configured */
    USB_STATE_CONFIGURED,    /**< USB is configured by host */
    USB_STATE_SUSPENDED,     /**< USB is suspended */
    USB_STATE_ERROR          /**< USB is in error state */
} usb_manager_state_t;

/**
 * @brief USB event types for callbacks
 */
typedef enum {
    USB_EVENT_CONFIGURED,    /**< Device has been configured by host */
    USB_EVENT_SUSPENDED,     /**< Device has been suspended */
    USB_EVENT_RESUMED,       /**< Device has been resumed */
    USB_EVENT_RESET,         /**< USB reset received */
    USB_EVENT_DISCONNECTED,  /**< Device disconnected */
    USB_EVENT_ERROR          /**< USB error occurred */
} usb_manager_event_t;

/**
 * @brief USB event callback function type
 * 
 * @param event The USB event that occurred
 * @param user_data User data pointer registered with the callback
 */
typedef void (*usb_manager_event_cb_t)(usb_manager_event_t event, void *user_data);

/**
 * @brief USB manager statistics
 */
typedef struct {
    uint32_t configured_count;
    uint32_t suspended_count;
    uint32_t resumed_count; 
    uint32_t reset_count;  
    uint32_t error_count;
} usb_manager_stats_t;

/**
 * @brief Maximum number of event callbacks that can be registered
 */
#define USB_MANAGER_MAX_CALLBACKS 4

/**
 * @brief Initialize the USB manager
 * 
 * This function must be called before any other USB manager functions.
 * It initializes the USB device stack and prepares the manager for operation.
 * 
 * @return 0 on success, negative errno on failure
 * @retval -EALREADY USB manager already initialized
 * @retval -ENODEV USB device not available
 * @retval -ENOMEM Out of memory
 */
int usb_manager_init(void);

/**
 * @brief Deinitialize the USB manager
 * 
 * Disables USB if enabled, unregisters all callbacks, and cleans up resources.
 * After calling this function, usb_manager_init() must be called again before
 * using any other USB manager functions.
 * 
 * @return 0 on success, negative errno on failure
 * @retval -EINVAL USB manager not initialized
 */
int usb_manager_deinit(void);

/**
 * @brief Enable the USB device
 * 
 * Enables the USB device and makes it visible to the host. The device will
 * enumerate and become available for communication.
 * 
 * @return 0 on success, negative errno on failure
 * @retval -EINVAL USB manager not initialized
 * @retval -EALREADY USB already enabled
 * @retval -EIO USB enable operation failed
 */
int usb_manager_enable(void);

/**
 * @brief Disable the USB device
 * 
 * Disables the USB device and disconnects from the host. The device will
 * no longer be visible to the host.
 * 
 * @return 0 on success, negative errno on failure
 * @retval -EINVAL USB manager not initialized or not enabled
 * @retval -EIO USB disable operation failed
 */
int usb_manager_disable(void);

/**
 * @brief Register a callback for USB events
 * 
 * Registers a callback function that will be called when USB events occur.
 * Multiple callbacks can be registered (up to USB_MANAGER_MAX_CALLBACKS).
 * 
 * @param callback Callback function pointer
 * @param user_data User data pointer that will be passed to the callback
 * 
 * @return Callback handle (>= 0) on success, negative errno on failure
 * @retval -EINVAL Invalid callback pointer or USB manager not initialized
 * @retval -ENOMEM Maximum number of callbacks already registered
 */
int usb_manager_register_callback(usb_manager_event_cb_t callback, void *user_data);

/**
 * @brief Unregister a USB event callback
 * 
 * Removes a previously registered callback using its handle.
 * 
 * @param callback_handle Handle returned by usb_manager_register_callback()
 * 
 * @return 0 on success, negative errno on failure
 * @retval -EINVAL Invalid callback handle or USB manager not initialized
 */
int usb_manager_unregister_callback(int callback_handle);

/**
 * @brief Get current USB device state
 * 
 * @return Current USB device state
 */
usb_manager_state_t usb_manager_get_state(void);

/**
 * @brief Get USB device state as a string
 * 
 * @param state The state to convert to string
 * @return String representation of the state
 */
const char *usb_manager_state_to_string(usb_manager_state_t state);

/**
 * @brief Get USB event as a string
 * 
 * @param event The event to convert to string
 * @return String representation of the event
 */
const char *usb_manager_event_to_string(usb_manager_event_t event);

/**
 * @brief Check if USB device is configured
 * 
 * @return true if USB is configured by host, false otherwise
 */
bool usb_manager_is_configured(void);

/**
 * @brief Check if USB device is enabled
 * 
 * @return true if USB is enabled, false otherwise
 */
bool usb_manager_is_enabled(void);

/**
 * @brief Get USB manager statistics
 * 
 * @param stats Pointer to statistics structure to fill
 * 
 * @return 0 on success, negative errno on failure
 * @retval -EINVAL Invalid stats pointer or USB manager not initialized
 */
int usb_manager_get_stats(usb_manager_stats_t *stats);

/**
 * @brief Reset USB manager statistics
 * 
 * Resets all statistics counters to zero.
 * 
 * @return 0 on success, negative errno on failure
 * @retval -EINVAL USB manager not initialized
 */
int usb_manager_reset_stats(void);

/**
 * @brief Get the USB device context
 * 
 * Returns the underlying USB device context for advanced operations.
 * This should be used carefully and only when necessary.
 * 
 * @return Pointer to USB device context, or NULL if not initialized
 */
struct usbd_context *usb_manager_get_context(void);

/**
 * @brief Trigger a USB remote wakeup (if supported)
 * 
 * Attempts to wake up the host from suspend state. Only works if
 * the device is suspended and remote wakeup is enabled.
 * 
 * @return 0 on success, negative errno on failure
 * @retval -EINVAL USB manager not initialized
 * @retval -ENOTSUP Remote wakeup not supported
 * @retval -EAGAIN Device not in suspended state
 */
int usb_manager_remote_wakeup(void);

/**
 * @brief Finalize USB manager after classes are registered
 */
int usb_manager_finalize(void);

/**
 * @brief Get if usb manager is initialized
 */
int usb_manager_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_MANAGER_H_ */