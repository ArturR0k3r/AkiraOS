/**
 * @file bt_hid_service.h
 * @brief Bluetooth HID GATT Service Implementation
 * 
 * Implements HID over GATT (HOGP - HID over GATT Profile) with:
 * - HID Service (0x1812)
 * - Battery Service (0x180F) 
 * - Device Information Service (0x180A)
 */

#ifndef AKIRA_BT_HID_SERVICE_H
#define AKIRA_BT_HID_SERVICE_H

#include "../hid/hid_common.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HID protocol mode
 */
typedef enum {
    BT_HID_PROTOCOL_BOOT = 0,
    BT_HID_PROTOCOL_REPORT = 1
} bt_hid_protocol_mode_t;

/**
 * @brief HID control point commands
 */
typedef enum {
    BT_HID_CP_SUSPEND = 0,
    BT_HID_CP_EXIT_SUSPEND = 1
} bt_hid_control_point_t;

/**
 * @brief Initialize BT HID service
 * @return 0 on success, negative error code on failure
 */
int bt_hid_service_init(void);

/**
 * @brief Register HID service with Bluetooth stack
 * @return 0 on success, negative error code on failure
 */
int bt_hid_service_register(void);

/**
 * @brief Send keyboard input report
 * @param report Keyboard report
 * @return 0 on success, negative error code on failure
 */
int bt_hid_service_send_keyboard_report(const hid_keyboard_report_t *report);

/**
 * @brief Send gamepad input report
 * @param report Gamepad report
 * @return 0 on success, negative error code on failure
 */
int bt_hid_service_send_gamepad_report(const hid_gamepad_report_t *report);

/**
 * @brief Check if HID service is connected
 * @return true if connected, false otherwise
 */
bool bt_hid_service_is_connected(void);

/**
 * @brief Get current protocol mode
 * @return Protocol mode (boot or report)
 */
bt_hid_protocol_mode_t bt_hid_service_get_protocol_mode(void);

/**
 * @brief Set battery level (0-100%)
 * @param level Battery level percentage
 * @return 0 on success
 */
int bt_hid_service_set_battery_level(uint8_t level);

/**
 * @brief Get connection handle
 * @return Connection handle or NULL if not connected
 */
void *bt_hid_service_get_conn(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BT_HID_SERVICE_H */
