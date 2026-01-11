/**
 * @file akira_gui_api.c
 * @brief LVGL GUI API implementation for WASM
 * 
 * Wraps LVGL functions for WebAssembly applications.
 */


#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_LVGL)
#include <lvgl.h>

LOG_MODULE_REGISTER(akira_gui_api, LOG_LEVEL_INF);

/* TODO: Add capability checks per  */

/*===========================================================================*/
/* Screen Management                                                         */
/*===========================================================================*/

gui_obj_t gui_screen_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    return (gui_obj_t)screen;
}

void gui_screen_load(gui_obj_t screen)
{
    if (!screen) return;
    lv_scr_load((lv_obj_t*)screen);
}

gui_obj_t gui_screen_active(void)
{
    return (gui_obj_t)lv_scr_act();
}

/*===========================================================================*/
/* Label Widget                                                              */
/*===========================================================================*/

gui_obj_t gui_label_create(gui_obj_t parent)
{
    lv_obj_t *lv_parent = parent ? (lv_obj_t*)parent : lv_scr_act();
    lv_obj_t *label = lv_label_create(lv_parent);
    return (gui_obj_t)label;
}

void gui_label_set_text(gui_obj_t label, const char *text)
{
    if (!label || !text) return;
    lv_label_set_text((lv_obj_t*)label, text);
}

void gui_label_set_text_fmt(gui_obj_t label, const char *fmt, ...)
{
    if (!label || !fmt) return;
    
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    lv_label_set_text((lv_obj_t*)label, buffer);
}

/*===========================================================================*/
/* Button Widget                                                             */
/*===========================================================================*/

gui_obj_t gui_button_create(gui_obj_t parent)
{
    lv_obj_t *lv_parent = parent ? (lv_obj_t*)parent : lv_scr_act();
    lv_obj_t *btn = lv_btn_create(lv_parent);
    return (gui_obj_t)btn;
}

gui_obj_t gui_button_set_label(gui_obj_t btn, const char *text)
{
    if (!btn || !text) return NULL;
    
    lv_obj_t *label = lv_label_create((lv_obj_t*)btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    
    return (gui_obj_t)label;
}

/* Event callback wrapper */
static void button_event_wrapper(lv_event_t *e)
{
    gui_event_cb_t cb = (gui_event_cb_t)lv_event_get_user_data(e);
    if (!cb) return;
    
    lv_obj_t *obj = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    
    gui_event_type_t event_type;
    switch (code) {
        case LV_EVENT_CLICKED:
            event_type = GUI_EVENT_CLICKED;
            break;
        case LV_EVENT_PRESSED:
            event_type = GUI_EVENT_PRESSED;
            break;
        case LV_EVENT_RELEASED:
            event_type = GUI_EVENT_RELEASED;
            break;
        case LV_EVENT_VALUE_CHANGED:
            event_type = GUI_EVENT_VALUE_CHANGED;
            break;
        case LV_EVENT_FOCUSED:
            event_type = GUI_EVENT_FOCUSED;
            break;
        case LV_EVENT_DEFOCUSED:
            event_type = GUI_EVENT_DEFOCUSED;
            break;
        default:
            return;
    }
    
    cb((gui_obj_t)obj, event_type);
}

void gui_button_add_event_cb(gui_obj_t btn, gui_event_cb_t cb)
{
    if (!btn || !cb) return;
    
    lv_obj_add_event_cb((lv_obj_t*)btn, button_event_wrapper, 
                        LV_EVENT_CLICKED, (void*)cb);
}

/*===========================================================================*/
/* Slider Widget                                                             */
/*===========================================================================*/

gui_obj_t gui_slider_create(gui_obj_t parent)
{
    lv_obj_t *lv_parent = parent ? (lv_obj_t*)parent : lv_scr_act();
    lv_obj_t *slider = lv_slider_create(lv_parent);
    return (gui_obj_t)slider;
}

void gui_slider_set_range(gui_obj_t slider, int32_t min, int32_t max)
{
    if (!slider) return;
    lv_slider_set_range((lv_obj_t*)slider, min, max);
}

void gui_slider_set_value(gui_obj_t slider, int32_t value, bool animated)
{
    if (!slider) return;
    lv_slider_set_value((lv_obj_t*)slider, value, 
                        animated ? LV_ANIM_ON : LV_ANIM_OFF);
}

int32_t gui_slider_get_value(gui_obj_t slider)
{
    if (!slider) return 0;
    return lv_slider_get_value((lv_obj_t*)slider);
}

void gui_slider_add_event_cb(gui_obj_t slider, gui_event_cb_t cb)
{
    if (!slider || !cb) return;
    
    lv_obj_add_event_cb((lv_obj_t*)slider, button_event_wrapper,
                        LV_EVENT_VALUE_CHANGED, (void*)cb);
}

/*===========================================================================*/
/* Image Widget                                                              */
/*===========================================================================*/

gui_obj_t gui_img_create(gui_obj_t parent)
{
    lv_obj_t *lv_parent = parent ? (lv_obj_t*)parent : lv_scr_act();
    lv_obj_t *img = lv_img_create(lv_parent);
    return (gui_obj_t)img;
}

void gui_img_set_src(gui_obj_t img, const void *data, int len)
{
    if (!img || !data) return;
    
    /* TODO: Convert data to LVGL image descriptor */
    /* For now, assume data is already in correct format */
    lv_img_set_src((lv_obj_t*)img, data);
}

/*===========================================================================*/
/* Object Properties                                                         */
/*===========================================================================*/

void gui_obj_set_pos(gui_obj_t obj, int16_t x, int16_t y)
{
    if (!obj) return;
    lv_obj_set_pos((lv_obj_t*)obj, x, y);
}

void gui_obj_set_size(gui_obj_t obj, int16_t w, int16_t h)
{
    if (!obj) return;
    lv_obj_set_size((lv_obj_t*)obj, w, h);
}

void gui_obj_align(gui_obj_t obj, gui_align_t align)
{
    if (!obj) return;
    
    lv_align_t lv_align;
    switch (align) {
        case GUI_ALIGN_CENTER:       lv_align = LV_ALIGN_CENTER; break;
        case GUI_ALIGN_TOP_LEFT:     lv_align = LV_ALIGN_TOP_LEFT; break;
        case GUI_ALIGN_TOP_MID:      lv_align = LV_ALIGN_TOP_MID; break;
        case GUI_ALIGN_TOP_RIGHT:    lv_align = LV_ALIGN_TOP_RIGHT; break;
        case GUI_ALIGN_BOTTOM_LEFT:  lv_align = LV_ALIGN_BOTTOM_LEFT; break;
        case GUI_ALIGN_BOTTOM_MID:   lv_align = LV_ALIGN_BOTTOM_MID; break;
        case GUI_ALIGN_BOTTOM_RIGHT: lv_align = LV_ALIGN_BOTTOM_RIGHT; break;
        case GUI_ALIGN_LEFT_MID:     lv_align = LV_ALIGN_LEFT_MID; break;
        case GUI_ALIGN_RIGHT_MID:    lv_align = LV_ALIGN_RIGHT_MID; break;
        default:                     lv_align = LV_ALIGN_CENTER; break;
    }
    
    lv_obj_align((lv_obj_t*)obj, lv_align, 0, 0);
}

void gui_obj_set_hidden(gui_obj_t obj, bool hidden)
{
    if (!obj) return;
    
    if (hidden) {
        lv_obj_add_flag((lv_obj_t*)obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag((lv_obj_t*)obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/*===========================================================================*/
/* Styling                                                                   */
/*===========================================================================*/

void gui_obj_set_style_bg_color(gui_obj_t obj, uint16_t color)
{
    if (!obj) return;
    
    lv_color_t lv_color = lv_color_hex(color);
    lv_obj_set_style_bg_color((lv_obj_t*)obj, lv_color, 0);
}

void gui_obj_set_style_text_color(gui_obj_t obj, uint16_t color)
{
    if (!obj) return;
    
    lv_color_t lv_color = lv_color_hex(color);
    lv_obj_set_style_text_color((lv_obj_t*)obj, lv_color, 0);
}

void gui_obj_set_style_border_width(gui_obj_t obj, int16_t width)
{
    if (!obj) return;
    lv_obj_set_style_border_width((lv_obj_t*)obj, width, 0);
}

/*===========================================================================*/
/* Animations                                                                */
/*===========================================================================*/

void gui_obj_fade_in(gui_obj_t obj, uint32_t time_ms)
{
    if (!obj) return;
    
    lv_obj_t *lv_obj = (lv_obj_t*)obj;
    lv_obj_set_style_opa(lv_obj, LV_OPA_TRANSP, 0);
    
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, lv_obj);
    lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&anim, time_ms);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_start(&anim);
}

void gui_obj_fade_out(gui_obj_t obj, uint32_t time_ms)
{
    if (!obj) return;
    
    lv_obj_t *lv_obj = (lv_obj_t*)obj;
    
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, lv_obj);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&anim, time_ms);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_start(&anim);
}

void gui_obj_move_to(gui_obj_t obj, int16_t x, int16_t y, uint32_t time_ms)
{
    if (!obj) return;
    
    lv_obj_t *lv_obj = (lv_obj_t*)obj;
    
    /* X animation */
    lv_anim_t anim_x;
    lv_anim_init(&anim_x);
    lv_anim_set_var(&anim_x, lv_obj);
    lv_anim_set_values(&anim_x, lv_obj_get_x(lv_obj), x);
    lv_anim_set_time(&anim_x, time_ms);
    lv_anim_set_exec_cb(&anim_x, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_start(&anim_x);
    
    /* Y animation */
    lv_anim_t anim_y;
    lv_anim_init(&anim_y);
    lv_anim_set_var(&anim_y, lv_obj);
    lv_anim_set_values(&anim_y, lv_obj_get_y(lv_obj), y);
    lv_anim_set_time(&anim_y, time_ms);
    lv_anim_set_exec_cb(&anim_y, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_start(&anim_y);
}

/*===========================================================================*/
/* Task Handler                                                              */
/*===========================================================================*/

void gui_task_handler(void)
{
    lv_task_handler();
}

#else /* !CONFIG_LVGL */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_gui_api, LOG_LEVEL_WRN);

/* Stub implementations when LVGL not enabled */
gui_obj_t gui_screen_create(void) { return NULL; }
void gui_screen_load(gui_obj_t screen) { }
gui_obj_t gui_screen_active(void) { return NULL; }
gui_obj_t gui_label_create(gui_obj_t parent) { return NULL; }
void gui_label_set_text(gui_obj_t label, const char *text) { }
void gui_label_set_text_fmt(gui_obj_t label, const char *fmt, ...) { }
gui_obj_t gui_button_create(gui_obj_t parent) { return NULL; }
gui_obj_t gui_button_set_label(gui_obj_t btn, const char *text) { return NULL; }
void gui_button_add_event_cb(gui_obj_t btn, gui_event_cb_t cb) { }
gui_obj_t gui_slider_create(gui_obj_t parent) { return NULL; }
void gui_slider_set_range(gui_obj_t slider, int32_t min, int32_t max) { }
void gui_slider_set_value(gui_obj_t slider, int32_t value, bool animated) { }
int32_t gui_slider_get_value(gui_obj_t slider) { return 0; }
void gui_slider_add_event_cb(gui_obj_t slider, gui_event_cb_t cb) { }
gui_obj_t gui_img_create(gui_obj_t parent) { return NULL; }
void gui_img_set_src(gui_obj_t img, const void *data, int len) { }
void gui_obj_set_pos(gui_obj_t obj, int16_t x, int16_t y) { }
void gui_obj_set_size(gui_obj_t obj, int16_t w, int16_t h) { }
void gui_obj_align(gui_obj_t obj, gui_align_t align) { }
void gui_obj_set_hidden(gui_obj_t obj, bool hidden) { }
void gui_obj_set_style_bg_color(gui_obj_t obj, uint16_t color) { }
void gui_obj_set_style_text_color(gui_obj_t obj, uint16_t color) { }
void gui_obj_set_style_border_width(gui_obj_t obj, int16_t width) { }
void gui_obj_fade_in(gui_obj_t obj, uint32_t time_ms) { }
void gui_obj_fade_out(gui_obj_t obj, uint32_t time_ms) { }
void gui_obj_move_to(gui_obj_t obj, int16_t x, int16_t y, uint32_t time_ms) { }
void gui_task_handler(void) { }

#endif /* CONFIG_LVGL */
