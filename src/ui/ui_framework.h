/**
 * @file ui_framework.h
 * @brief AkiraOS UI Framework
 * 
 * Lightweight UI framework for embedded displays.
 * Supports widgets, layouts, and touch/button input.
 */

#ifndef AKIRA_UI_FRAMEWORK_H
#define AKIRA_UI_FRAMEWORK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum widgets per screen
 */
#define UI_MAX_WIDGETS          32

/**
 * @brief Maximum screens
 */
#define UI_MAX_SCREENS          8

/**
 * @brief Widget types
 */
typedef enum {
	WIDGET_NONE = 0,
	WIDGET_LABEL,
	WIDGET_BUTTON,
	WIDGET_IMAGE,
	WIDGET_PROGRESS,
	WIDGET_SLIDER,
	WIDGET_CHECKBOX,
	WIDGET_LIST,
	WIDGET_CONTAINER,
	WIDGET_CUSTOM
} widget_type_t;

/**
 * @brief Text alignment
 */
typedef enum {
	ALIGN_LEFT   = 0,
	ALIGN_CENTER = 1,
	ALIGN_RIGHT  = 2
} text_align_t;

/**
 * @brief Widget handle
 */
typedef int widget_handle_t;

/**
 * @brief Screen handle
 */
typedef int screen_handle_t;

/**
 * @brief Color (RGB565)
 */
typedef uint16_t ui_color_t;

/**
 * @brief Point
 */
struct ui_point {
	int16_t x;
	int16_t y;
};

/**
 * @brief Rectangle
 */
struct ui_rect {
	int16_t x;
	int16_t y;
	uint16_t w;
	uint16_t h;
};

/**
 * @brief Widget style
 */
struct widget_style {
	ui_color_t bg_color;
	ui_color_t fg_color;
	ui_color_t border_color;
	uint8_t border_width;
	uint8_t padding;
	uint8_t corner_radius;
	text_align_t text_align;
	uint8_t font_size;  // 0=small, 1=medium, 2=large
};

/**
 * @brief Widget event types
 */
typedef enum {
	EVENT_NONE = 0,
	EVENT_PRESSED,
	EVENT_RELEASED,
	EVENT_VALUE_CHANGED,
	EVENT_FOCUS_IN,
	EVENT_FOCUS_OUT
} widget_event_t;

/**
 * @brief Event callback
 */
typedef void (*widget_callback_t)(widget_handle_t widget, widget_event_t event,
                                   void *user_data);

/**
 * @brief Initialize UI framework
 * @param width Screen width
 * @param height Screen height
 * @return 0 on success
 */
int ui_init(uint16_t width, uint16_t height);

/**
 * @brief Create a screen
 * @param name Screen name
 * @return Screen handle or negative error
 */
screen_handle_t ui_create_screen(const char *name);

/**
 * @brief Destroy a screen
 * @param screen Screen handle
 * @return 0 on success
 */
int ui_destroy_screen(screen_handle_t screen);

/**
 * @brief Set active screen
 * @param screen Screen handle
 * @return 0 on success
 */
int ui_set_screen(screen_handle_t screen);

/**
 * @brief Get current screen
 * @return Screen handle
 */
screen_handle_t ui_get_current_screen(void);

/**
 * @brief Create label widget
 * @param screen Parent screen
 * @param rect Position and size
 * @param text Label text
 * @return Widget handle or negative error
 */
widget_handle_t ui_create_label(screen_handle_t screen, struct ui_rect rect,
                                 const char *text);

/**
 * @brief Create button widget
 * @param screen Parent screen
 * @param rect Position and size
 * @param text Button text
 * @param callback Click callback
 * @param user_data User context
 * @return Widget handle or negative error
 */
widget_handle_t ui_create_button(screen_handle_t screen, struct ui_rect rect,
                                  const char *text, widget_callback_t callback,
                                  void *user_data);

/**
 * @brief Create progress bar widget
 * @param screen Parent screen
 * @param rect Position and size
 * @param value Initial value (0-100)
 * @return Widget handle or negative error
 */
widget_handle_t ui_create_progress(screen_handle_t screen, struct ui_rect rect,
                                    uint8_t value);

/**
 * @brief Create image widget
 * @param screen Parent screen
 * @param rect Position and size
 * @param image_data RGB565 image data
 * @return Widget handle or negative error
 */
widget_handle_t ui_create_image(screen_handle_t screen, struct ui_rect rect,
                                 const uint16_t *image_data);

/**
 * @brief Destroy widget
 * @param widget Widget handle
 * @return 0 on success
 */
int ui_destroy_widget(widget_handle_t widget);

/**
 * @brief Set widget text
 * @param widget Widget handle
 * @param text New text
 * @return 0 on success
 */
int ui_set_text(widget_handle_t widget, const char *text);

/**
 * @brief Set widget value
 * @param widget Widget handle
 * @param value New value
 * @return 0 on success
 */
int ui_set_value(widget_handle_t widget, int value);

/**
 * @brief Get widget value
 * @param widget Widget handle
 * @return Widget value
 */
int ui_get_value(widget_handle_t widget);

/**
 * @brief Set widget style
 * @param widget Widget handle
 * @param style Style settings
 * @return 0 on success
 */
int ui_set_style(widget_handle_t widget, const struct widget_style *style);

/**
 * @brief Set widget visibility
 * @param widget Widget handle
 * @param visible true to show
 * @return 0 on success
 */
int ui_set_visible(widget_handle_t widget, bool visible);

/**
 * @brief Set widget enabled state
 * @param widget Widget handle
 * @param enabled true to enable
 * @return 0 on success
 */
int ui_set_enabled(widget_handle_t widget, bool enabled);

/**
 * @brief Move widget
 * @param widget Widget handle
 * @param x New X position
 * @param y New Y position
 * @return 0 on success
 */
int ui_move_widget(widget_handle_t widget, int16_t x, int16_t y);

/**
 * @brief Process touch event
 * @param x Touch X
 * @param y Touch Y
 * @param pressed true if pressed
 * @return true if event was handled
 */
bool ui_process_touch(int16_t x, int16_t y, bool pressed);

/**
 * @brief Process button event
 * @param button Button ID (0=left, 1=up, 2=right, 3=down, 4=select)
 * @param pressed true if pressed
 * @return true if event was handled
 */
bool ui_process_button(uint8_t button, bool pressed);

/**
 * @brief Render current screen
 * @return 0 on success
 */
int ui_render(void);

/**
 * @brief Invalidate widget (mark for redraw)
 * @param widget Widget handle (or -1 for all)
 */
void ui_invalidate(widget_handle_t widget);

/**
 * @brief Set framebuffer for rendering
 * @param buffer RGB565 framebuffer
 */
void ui_set_framebuffer(uint16_t *buffer);

/* Color helpers */
#define UI_RGB(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

/* Common colors */
#define UI_BLACK       0x0000
#define UI_WHITE       0xFFFF
#define UI_RED         0xF800
#define UI_GREEN       0x07E0
#define UI_BLUE        0x001F
#define UI_GRAY        0x7BEF
#define UI_DARK_GRAY   0x39E7
#define UI_LIGHT_GRAY  0xC618

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_UI_FRAMEWORK_H */
