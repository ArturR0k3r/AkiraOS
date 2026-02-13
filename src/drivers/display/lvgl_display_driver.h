/**
 * @file lvgl_display_driver.h
 * @brief LVGL display driver for ILI9341
 */

#ifndef LVGL_DISPLAY_DRIVER_H
#define LVGL_DISPLAY_DRIVER_H

#include <stdint.h>

#if defined(CONFIG_LVGL)
#include <lvgl.h>
#endif

/**
 * @brief Initialize LVGL display driver
 * 
 * Sets up LVGL library and connects it to ILI9341 display.
 * 
 * @return 0 on success, negative error code on failure
 */
int lvgl_display_init(void);

/**
 * @brief Start LVGL tick timer
 * 
 * Starts 1ms periodic timer for LVGL internal timekeeping.
 */
void lvgl_start_tick(void);

/**
 * @brief Get LVGL display object
 * 
 * @return Display object pointer, or NULL if LVGL not enabled
 */
#if defined(CONFIG_LVGL)
lv_disp_t *lvgl_get_display(void);
#else
void *lvgl_get_display(void);
#endif

#endif /* LVGL_DISPLAY_DRIVER_H */
