/**
 * @file ui_framework.c
 * @brief AkiraOS UI Framework Implementation
 * 
 * Lightweight widget-based UI for embedded displays.
 * Optimized for low memory and CPU usage.
 */

#include "ui_framework.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ui, CONFIG_AKIRA_LOG_LEVEL);

/* Widget text buffer size */
#define WIDGET_TEXT_MAX     64

/* Widget structure */
struct widget {
	bool in_use;
	widget_type_t type;
	screen_handle_t screen;
	struct ui_rect rect;
	struct widget_style style;
	bool visible;
	bool enabled;
	bool dirty;  // Needs redraw
	
	/* Content */
	char text[WIDGET_TEXT_MAX];
	int value;
	const uint16_t *image_data;
	
	/* Events */
	widget_callback_t callback;
	void *user_data;
	
	/* Focus/selection state */
	bool focused;
	bool pressed;
};

/* Screen structure */
struct screen {
	bool in_use;
	char name[32];
	widget_handle_t widgets[UI_MAX_WIDGETS];
	int widget_count;
	ui_color_t bg_color;
	widget_handle_t focus_widget;
};

/* UI state */
static struct {
	bool initialized;
	uint16_t width;
	uint16_t height;
	
	struct widget widgets[UI_MAX_WIDGETS];
	struct screen screens[UI_MAX_SCREENS];
	
	screen_handle_t current_screen;
	uint16_t *framebuffer;
	
	/* Default style */
	struct widget_style default_style;
} ui_state;

/**
 * @brief Get widget by handle
 */
static struct widget *get_widget(widget_handle_t handle)
{
	if (handle < 0 || handle >= UI_MAX_WIDGETS) {
		return NULL;
	}
	if (!ui_state.widgets[handle].in_use) {
		return NULL;
	}
	return &ui_state.widgets[handle];
}

/**
 * @brief Get screen by handle
 */
static struct screen *get_screen(screen_handle_t handle)
{
	if (handle < 0 || handle >= UI_MAX_SCREENS) {
		return NULL;
	}
	if (!ui_state.screens[handle].in_use) {
		return NULL;
	}
	return &ui_state.screens[handle];
}

/**
 * @brief Find free widget slot
 */
static widget_handle_t find_free_widget(void)
{
	for (int i = 0; i < UI_MAX_WIDGETS; i++) {
		if (!ui_state.widgets[i].in_use) {
			return i;
		}
	}
	return -1;
}

/**
 * @brief Find free screen slot
 */
static screen_handle_t find_free_screen(void)
{
	for (int i = 0; i < UI_MAX_SCREENS; i++) {
		if (!ui_state.screens[i].in_use) {
			return i;
		}
	}
	return -1;
}

/**
 * @brief Draw filled rectangle
 */
static void draw_rect(int16_t x, int16_t y, uint16_t w, uint16_t h, ui_color_t color)
{
	// TODO: Implement rectangle drawing
	// - Clip to screen bounds
	// - Fill framebuffer
	
	if (!ui_state.framebuffer) {
		return;
	}
	
	/* Clipping */
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > ui_state.width) { w = ui_state.width - x; }
	if (y + h > ui_state.height) { h = ui_state.height - y; }
	
	for (int row = 0; row < h; row++) {
		uint16_t *line = &ui_state.framebuffer[(y + row) * ui_state.width + x];
		for (int col = 0; col < w; col++) {
			line[col] = color;
		}
	}
}

/**
 * @brief Draw text
 */
static void draw_text(int16_t x, int16_t y, const char *text, ui_color_t color,
                      uint8_t font_size)
{
	// TODO: Implement text rendering
	// - Use bitmap font
	// - Support different sizes
	// - Handle clipping
	
	(void)x;
	(void)y;
	(void)text;
	(void)color;
	(void)font_size;
	
	LOG_DBG("draw_text: '%s' at (%d,%d)", text, x, y);
}

/**
 * @brief Render a widget
 */
static void render_widget(struct widget *w)
{
	if (!w->visible || !w->dirty) {
		return;
	}
	
	// TODO: Implement proper widget rendering
	// - Background
	// - Border
	// - Content (text, image, progress bar)
	// - Focus indicator
	
	struct ui_rect *r = &w->rect;
	
	/* Background */
	draw_rect(r->x, r->y, r->w, r->h, w->style.bg_color);
	
	/* Border */
	if (w->style.border_width > 0) {
		// TODO: Draw border
	}
	
	/* Content based on type */
	switch (w->type) {
	case WIDGET_LABEL:
		draw_text(r->x + w->style.padding, r->y + w->style.padding,
		          w->text, w->style.fg_color, w->style.font_size);
		break;
		
	case WIDGET_BUTTON:
		draw_text(r->x + r->w/2, r->y + r->h/2,
		          w->text, w->style.fg_color, w->style.font_size);
		break;
		
	case WIDGET_PROGRESS:
		{
			uint16_t filled_w = (r->w - 4) * w->value / 100;
			draw_rect(r->x + 2, r->y + 2, filled_w, r->h - 4, w->style.fg_color);
		}
		break;
		
	case WIDGET_IMAGE:
		// TODO: Blit image data
		break;
		
	default:
		break;
	}
	
	w->dirty = false;
}

int ui_init(uint16_t width, uint16_t height)
{
	LOG_INF("Initializing UI framework (%dx%d)", width, height);
	
	ui_state.width = width;
	ui_state.height = height;
	
	for (int i = 0; i < UI_MAX_WIDGETS; i++) {
		ui_state.widgets[i].in_use = false;
	}
	
	for (int i = 0; i < UI_MAX_SCREENS; i++) {
		ui_state.screens[i].in_use = false;
	}
	
	ui_state.current_screen = -1;
	ui_state.framebuffer = NULL;
	
	/* Default style */
	ui_state.default_style.bg_color = UI_WHITE;
	ui_state.default_style.fg_color = UI_BLACK;
	ui_state.default_style.border_color = UI_GRAY;
	ui_state.default_style.border_width = 1;
	ui_state.default_style.padding = 4;
	ui_state.default_style.corner_radius = 0;
	ui_state.default_style.text_align = ALIGN_LEFT;
	ui_state.default_style.font_size = 1;
	
	ui_state.initialized = true;
	
	LOG_INF("UI framework initialized");
	return 0;
}

screen_handle_t ui_create_screen(const char *name)
{
	if (!ui_state.initialized) {
		return -ENODEV;
	}
	
	screen_handle_t handle = find_free_screen();
	if (handle < 0) {
		LOG_ERR("No free screen slots");
		return -ENOMEM;
	}
	
	struct screen *s = &ui_state.screens[handle];
	s->in_use = true;
	if (name) {
		strncpy(s->name, name, sizeof(s->name) - 1);
		s->name[sizeof(s->name) - 1] = '\0';
	}
	s->widget_count = 0;
	s->bg_color = UI_BLACK;
	s->focus_widget = -1;
	
	LOG_INF("Created screen '%s' (handle=%d)", s->name, handle);
	return handle;
}

int ui_destroy_screen(screen_handle_t screen)
{
	struct screen *s = get_screen(screen);
	if (!s) {
		return -ENOENT;
	}
	
	/* Destroy all widgets on this screen */
	for (int i = 0; i < s->widget_count; i++) {
		if (s->widgets[i] >= 0) {
			ui_destroy_widget(s->widgets[i]);
		}
	}
	
	s->in_use = false;
	
	if (ui_state.current_screen == screen) {
		ui_state.current_screen = -1;
	}
	
	LOG_INF("Destroyed screen '%s'", s->name);
	return 0;
}

int ui_set_screen(screen_handle_t screen)
{
	struct screen *s = get_screen(screen);
	if (!s) {
		return -ENOENT;
	}
	
	ui_state.current_screen = screen;
	
	/* Mark all widgets dirty */
	for (int i = 0; i < s->widget_count; i++) {
		struct widget *w = get_widget(s->widgets[i]);
		if (w) {
			w->dirty = true;
		}
	}
	
	LOG_DBG("Set active screen to '%s'", s->name);
	return 0;
}

screen_handle_t ui_get_current_screen(void)
{
	return ui_state.current_screen;
}

/**
 * @brief Create widget helper
 */
static widget_handle_t create_widget(screen_handle_t screen, widget_type_t type,
                                      struct ui_rect rect)
{
	struct screen *s = get_screen(screen);
	if (!s) {
		return -ENOENT;
	}
	
	if (s->widget_count >= UI_MAX_WIDGETS) {
		return -ENOMEM;
	}
	
	widget_handle_t handle = find_free_widget();
	if (handle < 0) {
		return -ENOMEM;
	}
	
	struct widget *w = &ui_state.widgets[handle];
	w->in_use = true;
	w->type = type;
	w->screen = screen;
	w->rect = rect;
	w->style = ui_state.default_style;
	w->visible = true;
	w->enabled = true;
	w->dirty = true;
	w->text[0] = '\0';
	w->value = 0;
	w->image_data = NULL;
	w->callback = NULL;
	w->user_data = NULL;
	w->focused = false;
	w->pressed = false;
	
	s->widgets[s->widget_count++] = handle;
	
	return handle;
}

widget_handle_t ui_create_label(screen_handle_t screen, struct ui_rect rect,
                                 const char *text)
{
	widget_handle_t handle = create_widget(screen, WIDGET_LABEL, rect);
	if (handle < 0) {
		return handle;
	}
	
	if (text) {
		strncpy(ui_state.widgets[handle].text, text, WIDGET_TEXT_MAX - 1);
	}
	
	/* Labels have transparent background by default */
	ui_state.widgets[handle].style.bg_color = UI_BLACK;
	ui_state.widgets[handle].style.fg_color = UI_WHITE;
	
	return handle;
}

widget_handle_t ui_create_button(screen_handle_t screen, struct ui_rect rect,
                                  const char *text, widget_callback_t callback,
                                  void *user_data)
{
	widget_handle_t handle = create_widget(screen, WIDGET_BUTTON, rect);
	if (handle < 0) {
		return handle;
	}
	
	struct widget *w = &ui_state.widgets[handle];
	if (text) {
		strncpy(w->text, text, WIDGET_TEXT_MAX - 1);
	}
	w->callback = callback;
	w->user_data = user_data;
	
	/* Button style */
	w->style.bg_color = UI_GRAY;
	w->style.fg_color = UI_WHITE;
	w->style.text_align = ALIGN_CENTER;
	
	return handle;
}

widget_handle_t ui_create_progress(screen_handle_t screen, struct ui_rect rect,
                                    uint8_t value)
{
	widget_handle_t handle = create_widget(screen, WIDGET_PROGRESS, rect);
	if (handle < 0) {
		return handle;
	}
	
	ui_state.widgets[handle].value = value > 100 ? 100 : value;
	
	/* Progress bar style */
	ui_state.widgets[handle].style.bg_color = UI_DARK_GRAY;
	ui_state.widgets[handle].style.fg_color = UI_GREEN;
	
	return handle;
}

widget_handle_t ui_create_image(screen_handle_t screen, struct ui_rect rect,
                                 const uint16_t *image_data)
{
	widget_handle_t handle = create_widget(screen, WIDGET_IMAGE, rect);
	if (handle < 0) {
		return handle;
	}
	
	ui_state.widgets[handle].image_data = image_data;
	
	return handle;
}

int ui_destroy_widget(widget_handle_t widget)
{
	struct widget *w = get_widget(widget);
	if (!w) {
		return -ENOENT;
	}
	
	w->in_use = false;
	return 0;
}

int ui_set_text(widget_handle_t widget, const char *text)
{
	struct widget *w = get_widget(widget);
	if (!w) {
		return -ENOENT;
	}
	
	if (text) {
		strncpy(w->text, text, WIDGET_TEXT_MAX - 1);
		w->text[WIDGET_TEXT_MAX - 1] = '\0';
	} else {
		w->text[0] = '\0';
	}
	
	w->dirty = true;
	return 0;
}

int ui_set_value(widget_handle_t widget, int value)
{
	struct widget *w = get_widget(widget);
	if (!w) {
		return -ENOENT;
	}
	
	w->value = value;
	w->dirty = true;
	return 0;
}

int ui_get_value(widget_handle_t widget)
{
	struct widget *w = get_widget(widget);
	if (!w) {
		return 0;
	}
	return w->value;
}

int ui_set_style(widget_handle_t widget, const struct widget_style *style)
{
	struct widget *w = get_widget(widget);
	if (!w || !style) {
		return -EINVAL;
	}
	
	w->style = *style;
	w->dirty = true;
	return 0;
}

int ui_set_visible(widget_handle_t widget, bool visible)
{
	struct widget *w = get_widget(widget);
	if (!w) {
		return -ENOENT;
	}
	
	w->visible = visible;
	w->dirty = true;
	return 0;
}

int ui_set_enabled(widget_handle_t widget, bool enabled)
{
	struct widget *w = get_widget(widget);
	if (!w) {
		return -ENOENT;
	}
	
	w->enabled = enabled;
	w->dirty = true;
	return 0;
}

int ui_move_widget(widget_handle_t widget, int16_t x, int16_t y)
{
	struct widget *w = get_widget(widget);
	if (!w) {
		return -ENOENT;
	}
	
	w->rect.x = x;
	w->rect.y = y;
	w->dirty = true;
	return 0;
}

bool ui_process_touch(int16_t x, int16_t y, bool pressed)
{
	if (!ui_state.initialized || ui_state.current_screen < 0) {
		return false;
	}
	
	struct screen *s = get_screen(ui_state.current_screen);
	if (!s) {
		return false;
	}
	
	// TODO: Implement touch handling
	// 1. Find widget at touch point
	// 2. Handle press/release events
	// 3. Call widget callbacks
	
	for (int i = 0; i < s->widget_count; i++) {
		struct widget *w = get_widget(s->widgets[i]);
		if (!w || !w->visible || !w->enabled) {
			continue;
		}
		
		/* Hit test */
		if (x >= w->rect.x && x < w->rect.x + w->rect.w &&
		    y >= w->rect.y && y < w->rect.y + w->rect.h) {
			
			if (pressed && !w->pressed) {
				w->pressed = true;
				w->dirty = true;
				if (w->callback) {
					w->callback(s->widgets[i], EVENT_PRESSED, w->user_data);
				}
				return true;
			} else if (!pressed && w->pressed) {
				w->pressed = false;
				w->dirty = true;
				if (w->callback) {
					w->callback(s->widgets[i], EVENT_RELEASED, w->user_data);
				}
				return true;
			}
		}
	}
	
	return false;
}

bool ui_process_button(uint8_t button, bool pressed)
{
	// TODO: Implement D-pad/button navigation
	// 0=left, 1=up, 2=right, 3=down, 4=select
	
	(void)button;
	(void)pressed;
	
	LOG_WRN("ui_process_button not implemented");
	return false;
}

int ui_render(void)
{
	if (!ui_state.initialized || ui_state.current_screen < 0) {
		return -ENODEV;
	}
	
	struct screen *s = get_screen(ui_state.current_screen);
	if (!s) {
		return -ENOENT;
	}
	
	/* Clear background */
	if (ui_state.framebuffer) {
		draw_rect(0, 0, ui_state.width, ui_state.height, s->bg_color);
	}
	
	/* Render all widgets */
	for (int i = 0; i < s->widget_count; i++) {
		struct widget *w = get_widget(s->widgets[i]);
		if (w) {
			render_widget(w);
		}
	}
	
	return 0;
}

void ui_invalidate(widget_handle_t widget)
{
	if (widget < 0) {
		/* Invalidate all */
		for (int i = 0; i < UI_MAX_WIDGETS; i++) {
			if (ui_state.widgets[i].in_use) {
				ui_state.widgets[i].dirty = true;
			}
		}
	} else {
		struct widget *w = get_widget(widget);
		if (w) {
			w->dirty = true;
		}
	}
}

void ui_set_framebuffer(uint16_t *buffer)
{
	ui_state.framebuffer = buffer;
}
