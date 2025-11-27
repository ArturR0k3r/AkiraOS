/**
 * @file hid_sim.h
 * @brief HID Simulation for native_sim Testing
 */

#ifndef AKIRA_HID_SIM_H
#define AKIRA_HID_SIM_H

#include "hid_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize HID simulation transport
 * @return 0 on success
 */
int hid_sim_init(void);

/**
 * @brief Get simulated transport operations
 * @return Transport operations pointer
 */
const hid_transport_ops_t *hid_sim_get_transport(void);

/**
 * @brief Simulate host connection
 */
void hid_sim_connect(void);

/**
 * @brief Simulate host disconnection
 */
void hid_sim_disconnect(void);

/**
 * @brief Get last keyboard report (for testing)
 * @return Pointer to last keyboard report
 */
const hid_keyboard_report_t *hid_sim_get_last_keyboard_report(void);

/**
 * @brief Get last gamepad report (for testing)
 * @return Pointer to last gamepad report
 */
const hid_gamepad_report_t *hid_sim_get_last_gamepad_report(void);

/**
 * @brief Send simulated output report (LED state, etc.)
 * @param data Output data
 * @param len Data length
 */
void hid_sim_send_output_report(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_HID_SIM_H */
