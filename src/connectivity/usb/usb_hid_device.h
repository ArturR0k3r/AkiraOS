/**
 * @file usb_hid_device.h
 * @brief USB HID Device Implementation
 * 
 * Implements USB HID class device with support for:
 * - Keyboard
 * - Gamepad
 * - Multiple report IDs
 */

#ifndef AKIRA_USB_HID_DEVICE_H
#define AKIRA_USB_HID_DEVICE_H

#include <stdint.h>
#include <stdbool.h>

/* Include Zephyr HID definitions first if USB is available */
#if defined(CONFIG_USB_DEVICE_STACK)
#include <zephyr/usb/class/hid.h>
#endif

#include "../hid/hid_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief USB HID output report callback
 * 
 * Called when host sends output report (e.g., LED state for keyboard)
 * 
 * @param report_id Report ID
 * @param data Report data
 * @param len Report data length
 * @param user_data User data pointer
 */
typedef void (*usb_hid_output_callback_t)(uint8_t report_id, const uint8_t *data, 
                                           uint16_t len, void *user_data);

/**
 * @brief Initialize USB HID device
 * @return 0 on success, negative error code on failure
 */
int usb_hid_device_init(void);

/**
 * @brief Deinitialize USB HID device
 * @return 0 on success
 */
int usb_hid_device_deinit(void);

/**
 * @brief Register USB HID class with USB stack
 * @return 0 on success, negative error code on failure
 */
int usb_hid_device_register(void);

/**
 * @brief Send keyboard input report
 * @param report Keyboard report
 * @return 0 on success, negative error code on failure
 */
int usb_hid_device_send_keyboard_report(const hid_keyboard_report_t *report);

/**
 * @brief Send gamepad input report
 * @param report Gamepad report
 * @return 0 on success, negative error code on failure
 */
int usb_hid_device_send_gamepad_report(const hid_gamepad_report_t *report);

/**
 * @brief Check if USB HID is ready to send
 * @return true if ready, false otherwise
 */
bool usb_hid_device_is_ready(void);

/**
 * @brief Register output report callback
 * @param callback Callback function
 * @param user_data User data pointer
 * @return 0 on success
 */
int usb_hid_device_register_output_callback(usb_hid_output_callback_t callback, 
                                             void *user_data);

/**
 * @brief Get USB HID interface ready state
 * @return true if interface is configured
 */
bool usb_hid_device_is_configured(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_USB_HID_DEVICE_H */
