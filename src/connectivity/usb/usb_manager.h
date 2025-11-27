/**
 * @file usb_manager.h
 * @brief USB Manager for AkiraOS
 *
 * Manages USB stack initialization and USB device classes.
 */

#ifndef AKIRA_USB_MANAGER_H
#define AKIRA_USB_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

/** USB states */
typedef enum {
    USB_STATE_OFF = 0,
    USB_STATE_INITIALIZING,
    USB_STATE_READY,
    USB_STATE_CONNECTED,
    USB_STATE_SUSPENDED,
    USB_STATE_ERROR
} usb_state_t;

/** USB device classes */
typedef enum {
    USB_CLASS_HID    = 0x01,
    USB_CLASS_CDC    = 0x02,  /* For serial/shell */
    USB_CLASS_MSC    = 0x04,  /* Mass storage (future) */
    USB_CLASS_ALL    = 0x07
} usb_class_t;

/** USB configuration */
typedef struct {
    const char *manufacturer;
    const char *product;
    const char *serial;
    uint16_t vendor_id;
    uint16_t product_id;
    usb_class_t classes;
} usb_config_t;

/** USB statistics */
typedef struct {
    usb_state_t state;
    uint32_t bytes_rx;
    uint32_t bytes_tx;
    bool vbus_present;
} usb_stats_t;

/*===========================================================================*/
/* Event Callbacks                                                           */
/*===========================================================================*/

typedef enum {
    USB_EVENT_CONNECTED,
    USB_EVENT_DISCONNECTED,
    USB_EVENT_SUSPENDED,
    USB_EVENT_RESUMED,
    USB_EVENT_ERROR
} usb_event_t;

typedef void (*usb_event_callback_t)(usb_event_t event, void *user_data);

/*===========================================================================*/
/* USB Manager API                                                           */
/*===========================================================================*/

/**
 * @brief Initialize USB manager
 * @param config Configuration (NULL for defaults)
 * @return 0 on success
 */
int usb_manager_init(const usb_config_t *config);

/**
 * @brief Deinitialize USB manager
 * @return 0 on success
 */
int usb_manager_deinit(void);

/**
 * @brief Enable USB device
 * @return 0 on success
 */
int usb_manager_enable(void);

/**
 * @brief Disable USB device
 * @return 0 on success
 */
int usb_manager_disable(void);

/**
 * @brief Get current USB state
 * @return USB state
 */
usb_state_t usb_manager_get_state(void);

/**
 * @brief Get USB statistics
 * @param stats Output buffer
 * @return 0 on success
 */
int usb_manager_get_stats(usb_stats_t *stats);

/**
 * @brief Check if USB is connected
 * @return true if connected
 */
bool usb_manager_is_connected(void);

/**
 * @brief Register event callback
 * @param callback Callback function
 * @param user_data User data
 * @return 0 on success
 */
int usb_manager_register_callback(usb_event_callback_t callback, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_USB_MANAGER_H */
