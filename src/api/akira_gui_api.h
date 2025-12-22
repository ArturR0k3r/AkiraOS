/**
 * @file akira_gui_api.h
 * @brief LVGL GUI API for WASM apps
 * 
 * High-level GUI API exposing LVGL widgets to WebAssembly applications.
 */

#ifndef AKIRA_GUI_API_H
#define AKIRA_GUI_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

/* Opaque handle for GUI objects */
typedef void* gui_obj_t;

/* Event types */
typedef enum {
    GUI_EVENT_CLICKED,
    GUI_EVENT_PRESSED,
    GUI_EVENT_RELEASED,
    GUI_EVENT_VALUE_CHANGED,
    GUI_EVENT_FOCUSED,
    GUI_EVENT_DEFOCUSED,
} gui_event_type_t;

/* Alignment constants */
typedef enum {
    GUI_ALIGN_CENTER = 0,
    GUI_ALIGN_TOP_LEFT,
    GUI_ALIGN_TOP_MID,
    GUI_ALIGN_TOP_RIGHT,
    GUI_ALIGN_BOTTOM_LEFT,
    GUI_ALIGN_BOTTOM_MID,
    GUI_ALIGN_BOTTOM_RIGHT,
    GUI_ALIGN_LEFT_MID,
    GUI_ALIGN_RIGHT_MID,
} gui_align_t;

/* Event callback type */
typedef void (*gui_event_cb_t)(gui_obj_t obj, gui_event_type_t event);

/*===========================================================================*/
/* Screen Management                                                         */
/*===========================================================================*/

/**
 * @brief Create a new screen
 * @return Screen object handle
 */
gui_obj_t gui_screen_create(void);

/**
 * @brief Load (display) a screen
 * @param screen Screen to load
 */
void gui_screen_load(gui_obj_t screen);

/**
 * @brief Get active screen
 * @return Active screen handle
 */
gui_obj_t gui_screen_active(void);

/*===========================================================================*/
/* Label Widget                                                              */
/*===========================================================================*/

/**
 * @brief Create label widget
 * @param parent Parent object (or NULL for active screen)
 * @return Label object handle
 */
gui_obj_t gui_label_create(gui_obj_t parent);

/**
 * @brief Set label text
 * @param label Label object
 * @param text Text string
 */
void gui_label_set_text(gui_obj_t label, const char *text);

/**
 * @brief Set label text with printf formatting
 * @param label Label object
 * @param fmt Format string
 * @param ... Variable arguments
 */
void gui_label_set_text_fmt(gui_obj_t label, const char *fmt, ...);

/*===========================================================================*/
/* Button Widget                                                             */
/*===========================================================================*/

/**
 * @brief Create button widget
 * @param parent Parent object (or NULL for active screen)
 * @return Button object handle
 */
gui_obj_t gui_button_create(gui_obj_t parent);

/**
 * @brief Set button label text
 * @param btn Button object
 * @param text Button label
 * @return Label object handle
 */
gui_obj_t gui_button_set_label(gui_obj_t btn, const char *text);

/**
 * @brief Add event callback to button
 * @param btn Button object
 * @param cb Callback function
 */
void gui_button_add_event_cb(gui_obj_t btn, gui_event_cb_t cb);

/*===========================================================================*/
/* Slider Widget                                                             */
/*===========================================================================*/

/**
 * @brief Create slider widget
 * @param parent Parent object (or NULL for active screen)
 * @return Slider object handle
 */
gui_obj_t gui_slider_create(gui_obj_t parent);

/**
 * @brief Set slider value range
 * @param slider Slider object
 * @param min Minimum value
 * @param max Maximum value
 */
void gui_slider_set_range(gui_obj_t slider, int32_t min, int32_t max);

/**
 * @brief Set slider value
 * @param slider Slider object
 * @param value New value
 * @param animated Animate transition
 */
void gui_slider_set_value(gui_obj_t slider, int32_t value, bool animated);

/**
 * @brief Get slider value
 * @param slider Slider object
 * @return Current value
 */
int32_t gui_slider_get_value(gui_obj_t slider);

/**
 * @brief Add event callback to slider
 * @param slider Slider object
 * @param cb Callback function
 */
void gui_slider_add_event_cb(gui_obj_t slider, gui_event_cb_t cb);

/*===========================================================================*/
/* Image Widget                                                              */
/*===========================================================================*/

/**
 * @brief Create image widget
 * @param parent Parent object (or NULL for active screen)
 * @return Image object handle
 */
gui_obj_t gui_img_create(gui_obj_t parent);

/**
 * @brief Set image source from memory
 * @param img Image object
 * @param data Image data buffer
 * @param len Data length
 */
void gui_img_set_src(gui_obj_t img, const void *data, int len);

/*===========================================================================*/
/* Object Properties (Common to all widgets)                                */
/*===========================================================================*/

/**
 * @brief Set object position
 * @param obj GUI object
 * @param x X coordinate
 * @param y Y coordinate
 */
void gui_obj_set_pos(gui_obj_t obj, int16_t x, int16_t y);

/**
 * @brief Set object size
 * @param obj GUI object
 * @param w Width
 * @param h Height
 */
void gui_obj_set_size(gui_obj_t obj, int16_t w, int16_t h);

/**
 * @brief Align object relative to parent
 * @param obj GUI object
 * @param align Alignment type
 */
void gui_obj_align(gui_obj_t obj, gui_align_t align);

/**
 * @brief Set object visibility
 * @param obj GUI object
 * @param visible True to show, false to hide
 */
void gui_obj_set_hidden(gui_obj_t obj, bool hidden);

/*===========================================================================*/
/* Styling                                                                   */
/*===========================================================================*/

/**
 * @brief Set background color
 * @param obj GUI object
 * @param color RGB565 color
 */
void gui_obj_set_style_bg_color(gui_obj_t obj, uint16_t color);

/**
 * @brief Set text color
 * @param obj GUI object
 * @param color RGB565 color
 */
void gui_obj_set_style_text_color(gui_obj_t obj, uint16_t color);

/**
 * @brief Set border width
 * @param obj GUI object
 * @param width Border width in pixels
 */
void gui_obj_set_style_border_width(gui_obj_t obj, int16_t width);

/*===========================================================================*/
/* Animations                                                                */
/*===========================================================================*/

/**
 * @brief Fade in object
 * @param obj GUI object
 * @param time_ms Animation duration in milliseconds
 */
void gui_obj_fade_in(gui_obj_t obj, uint32_t time_ms);

/**
 * @brief Fade out object
 * @param obj GUI object
 * @param time_ms Animation duration in milliseconds
 */
void gui_obj_fade_out(gui_obj_t obj, uint32_t time_ms);

/**
 * @brief Animate object movement
 * @param obj GUI object
 * @param x Target X coordinate
 * @param y Target Y coordinate
 * @param time_ms Animation duration in milliseconds
 */
void gui_obj_move_to(gui_obj_t obj, int16_t x, int16_t y, uint32_t time_ms);

/*===========================================================================*/
/* Task Handler                                                              */
/*===========================================================================*/

/**
 * @brief Process LVGL tasks
 * 
 * Call this periodically (every 5-10ms) from app main loop.
 */
void gui_task_handler(void);

#endif /* AKIRA_GUI_API_H */
