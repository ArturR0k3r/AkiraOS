/**
 * @file usb_hid.h
 * @brief USB HID Transport Header for HID Manager
 */

#ifndef USB_HID_H
#define USB_HID_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

/**
 * @brief Initialize and register USB HID transport with HID manager
 * 
 * This function should be called during system initialization to register
 * the USB HID transport with the HID manager. It does not initialize the
 * HID device itself - that happens when the HID manager calls the transport's
 * init function.
 * 
 * @return 0 on success, negative error code on failure
 * @retval -ENOMEM Failed to register (too many transports)
 * @retval -EINVAL Invalid parameters
 */
int usb_hid_transport_init(void);

/**
 * @brief Get the USB HID device
 * 
 * Returns the pointer to the USB HID device. This can be used for
 * direct access to the HID device if needed.
 * 
 * @return Pointer to HID device, or NULL if not initialized
 */
const struct device *usb_hid_get_device(void);

/**
 * @brief Check if USB HID is ready to send reports
 * 
 * Checks if the USB HID interface is configured and ready to send reports.
 * This is a combination of:
 * - Transport is initialized
 * - Transport is enabled
 * - USB is configured
 * - HID interface is ready
 * 
 * @return true if ready to send reports, false otherwise
 */
bool usb_hid_is_ready(void);

/**
 * @brief Get current HID protocol
 * 
 * Returns the current HID protocol in use:
 * - 0: Boot protocol (BIOS compatible)
 * - 1: Report protocol (full featured)
 * 
 * @return Current protocol (0 or 1)
 */
uint8_t usb_hid_get_protocol(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_HID_H */