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

#if defined(CONFIG_LVGL)
#include <lvgl.h>
/**
 * @brief Get the registered LVGL keypad input device.
 *
 * Use this to associate a keypad group with the LVGL focus manager.
 *
 * @return Pointer to lv_indev_t, or NULL if not initialised.
 */
lv_indev_t *lvgl_input_get_keypad(void);
#else
static inline void *lvgl_input_get_keypad(void) { return NULL; }
#endif

/**
 * @brief Register a callback fired on X-button long-press (HOME event).
 *
 * The OS shell installs this callback to detect the user intent to return
 * to the launcher from a running WASM app.
 *
 * @param cb        Function to call on HOME long-press.  May be NULL to clear.
 * @param user_data Opaque pointer passed back to @p cb.
 */
void lvgl_input_set_home_callback(void (*cb)(void *user_data), void *user_data);

#endif /* LVGL_INPUT_DRIVER_H */
