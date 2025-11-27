/**
 * @file bt_hid.h
 * @brief Bluetooth HID Service for AkiraOS
 *
 * BLE HID device implementation supporting keyboard and gamepad profiles.
 */

#ifndef AKIRA_BT_HID_H
#define AKIRA_BT_HID_H

#include "../hid/hid_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Bluetooth HID service
 * @return 0 on success
 */
int bt_hid_init(void);

/**
 * @brief Get BLE HID transport operations
 * @return Transport operations pointer
 */
const hid_transport_ops_t *bt_hid_get_transport(void);

/**
 * @brief Enable BLE HID service
 * @return 0 on success
 */
int bt_hid_enable(void);

/**
 * @brief Disable BLE HID service
 * @return 0 on success
 */
int bt_hid_disable(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_BT_HID_H */
