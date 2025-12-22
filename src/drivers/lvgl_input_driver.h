/**
 * @file lvgl_input_driver.h
 * @brief LVGL input driver for touch/buttons
 */

#ifndef LVGL_INPUT_DRIVER_H
#define LVGL_INPUT_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize LVGL input drivers
 * 
 * Sets up touch and button input devices for LVGL.
 * 
 * @return 0 on success, negative error code on failure
 */
int lvgl_input_init(void);

/**
 * @brief Update touch state
 * 
 * Call this from touch controller driver when touch state changes.
 * 
 * @param x X coordinate (0-319)
 * @param y Y coordinate (0-239)
 * @param pressed True if touched, false if released
 */
void lvgl_input_update_touch(int16_t x, int16_t y, bool pressed);

/**
 * @brief Update button state
 * 
 * Call this from button driver when button state changes.
 * 
 * @param buttons Button state bitmask
 */
void lvgl_input_update_buttons(uint32_t buttons);

#endif /* LVGL_INPUT_DRIVER_H */
