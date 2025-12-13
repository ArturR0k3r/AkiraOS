/**
 * @file hid_gamepad.h
 * @brief HID Gamepad Profile Implementation
 * 
 * Manages gamepad HID report state with support for:
 * - 16 buttons
 * - 4 analog axes (2 sticks)
 * - 2 analog triggers
 * - 8-way D-pad/hat
 */

#ifndef AKIRA_HID_GAMEPAD_H
#define AKIRA_HID_GAMEPAD_H

#include "hid_common.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize gamepad HID profile
 * @return 0 on success, negative error code on failure
 */
int hid_gamepad_init(void);

/**
 * @brief Deinitialize gamepad HID profile
 * @return 0 on success
 */
int hid_gamepad_deinit(void);

/**
 * @brief Press a button
 * @param button Button index (0-15)
 * @return 0 on success, -EINVAL if invalid button
 */
int hid_gamepad_press_button(uint8_t button);

/**
 * @brief Release a button
 * @param button Button index (0-15)
 * @return 0 on success
 */
int hid_gamepad_release_button(uint8_t button);

/**
 * @brief Release all buttons
 * @return 0 on success
 */
int hid_gamepad_release_all_buttons(void);

/**
 * @brief Set analog axis value
 * @param axis Axis index (see hid_gamepad_axis_t)
 * @param value Axis value (-32768 to 32767, 0 = center)
 * @return 0 on success
 */
int hid_gamepad_set_axis(hid_gamepad_axis_t axis, int16_t value);

/**
 * @brief Set trigger value (L2/R2)
 * @param trigger Trigger index (0=L2, 1=R2)
 * @param value Trigger value (0 to 32767, 0 = released)
 * @return 0 on success
 */
int hid_gamepad_set_trigger(uint8_t trigger, int16_t value);

/**
 * @brief Set D-pad direction
 * @param direction Direction (0-7) or -1 for center
 *   0 = Up, 1 = Up-Right, 2 = Right, 3 = Down-Right,
 *   4 = Down, 5 = Down-Left, 6 = Left, 7 = Up-Left
 * @return 0 on success
 */
int hid_gamepad_set_hat(int8_t direction);

/**
 * @brief Reset all gamepad inputs to neutral/centered
 * @return 0 on success
 */
int hid_gamepad_reset(void);

/**
 * @brief Get current gamepad report
 * @param report Pointer to report structure to fill
 * @return 0 on success
 */
int hid_gamepad_get_report(hid_gamepad_report_t *report);

/**
 * @brief Check if a button is currently pressed
 * @param button Button index (0-15)
 * @return true if pressed, false otherwise
 */
bool hid_gamepad_is_button_pressed(uint8_t button);

/**
 * @brief Get current button state bitmask
 * @return 16-bit bitmask of pressed buttons
 */
uint16_t hid_gamepad_get_button_state(void);

/**
 * @brief Get current axis value
 * @param axis Axis index
 * @return Axis value (-32768 to 32767)
 */
int16_t hid_gamepad_get_axis(hid_gamepad_axis_t axis);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_HID_GAMEPAD_H */
