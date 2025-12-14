/**
 * @file hid_keyboard.h
 * @brief HID Keyboard Profile Implementation
 * 
 * Manages keyboard HID report state and provides functions for
 * key press/release, modifier handling, and string typing.
 */

#ifndef AKIRA_HID_KEYBOARD_H
#define AKIRA_HID_KEYBOARD_H

#include "hid_common.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize keyboard HID profile
 * @return 0 on success, negative error code on failure
 */
int hid_keyboard_init(void);

/**
 * @brief Deinitialize keyboard HID profile
 * @return 0 on success
 */
int hid_keyboard_deinit(void);

/**
 * @brief Press a key (add to report)
 * @param keycode HID keycode (see HID_KEY_* in hid_common.h)
 * @return 0 on success, -ENOMEM if report full
 */
int hid_keyboard_press_key(uint8_t keycode);

/**
 * @brief Release a key (remove from report)
 * @param keycode HID keycode
 * @return 0 on success, -ENOENT if key not pressed
 */
int hid_keyboard_release_key(uint8_t keycode);

/**
 * @brief Clear all keys and modifiers (internal)
 * @return 0 on success
 */
int hid_keyboard_clear(void);

/**
 * @brief Set modifier keys bitmask (internal)
 * @param modifiers Bitmask of HID_MOD_* values
 * @return 0 on success
 */
int hid_keyboard_set_modifier(uint8_t modifiers);

/**
 * @brief Press a modifier key
 * @param modifier HID_MOD_* value
 * @return 0 on success
 */
int hid_keyboard_press_modifier(uint8_t modifier);

/**
 * @brief Release a modifier key
 * @param modifier HID_MOD_* value
 * @return 0 on success
 */
int hid_keyboard_release_modifier(uint8_t modifier);

/**
 * @brief Get current modifier state
 * @return Modifier bitmask
 */
uint8_t hid_keyboard_get_modifiers(void);

/**
 * @brief Type a string (sequences of key presses)
 * 
 * Automatically handles shifts for uppercase and special chars.
 * Blocks until complete or error occurs.
 * 
 * @param str Null-terminated string
 * @param send_callback Function to send reports (optional, uses default if NULL)
 * @return 0 on success, negative error code on failure
 */
int hid_keyboard_type_string(const char *str, int (*send_callback)(const hid_keyboard_report_t *));

/**
 * @brief Get current keyboard report
 * @param report Pointer to report structure to fill
 * @return 0 on success
 */
int hid_keyboard_get_report(hid_keyboard_report_t *report);

/**
 * @brief Check if a key is currently pressed
 * @param keycode HID keycode
 * @return true if pressed, false otherwise
 */
bool hid_keyboard_is_key_pressed(uint8_t keycode);

/**
 * @brief Get number of keys currently pressed
 * @return Number of keys (0-6)
 */
uint8_t hid_keyboard_get_pressed_count(void);

/**
 * @brief Convert ASCII character to HID keycode + modifier
 * @param ch ASCII character
 * @param modifier Output: required modifier (0 if none)
 * @return HID keycode, or HID_KEY_NONE if not mappable
 */
uint8_t hid_keyboard_ascii_to_keycode(char ch, uint8_t *modifier);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_HID_KEYBOARD_H */
