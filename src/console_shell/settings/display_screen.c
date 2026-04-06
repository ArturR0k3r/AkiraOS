/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_display_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_display_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file display_screen.c
 * @brief Display brightness slider and screen-off timeout spinbox.
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/display.h>


#include "../shell_theme.h"
#include "display_screen.h"

#define BRIGHTNESS_MIN  10
#define BRIGHTNESS_MAX  255
#define TIMEOUT_MIN_S   5
#define TIMEOUT_MAX_S   300

static lv_obj_t *g_screen;
static lv_obj_t *g_brightness_slider;
static lv_obj_t *g_brightness_val_label;
static lv_obj_t *g_timeout_spinbox;

/* ------------------------------------------------------------------ */
/* Hardware write                                                       */
/* ------------------------------------------------------------------ */

static void apply_brightness(int val)
{
#if defined(CONFIG_DISPLAY)
    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (device_is_ready(disp)) {
        uint8_t pct = (uint8_t)((val * 100U) / BRIGHTNESS_MAX);
        display_set_brightness(disp, pct);
    }
#endif
    LOG_DBG("Brightness set to %d", val);
}

/* ------------------------------------------------------------------ */
/* Back key                                                             */
/* ------------------------------------------------------------------ */

static void back_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_KEY) {
        uint32_t key = lv_indev_get_key(lv_indev_get_act());
        if (key == LV_KEY_ESC) {
            extern void settings_screen_load(void);
            settings_screen_load();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Slider callback                                                      */
/* ------------------------------------------------------------------ */

static void brightness_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    int val = lv_slider_get_value(g_brightness_slider);
    lv_label_set_text_fmt(g_brightness_val_label, "%d", val);
    apply_brightness(val);

    settings_save_one("akira/display/brightness", &val, sizeof(val));
}

/* ------------------------------------------------------------------ */
/* Spinbox callback                                                     */
/* ------------------------------------------------------------------ */

static void timeout_save_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    int32_t val = lv_spinbox_get_value(g_timeout_spinbox);
    settings_save_one("akira/display/timeout_s", &val, sizeof(val));
}

/* ------------------------------------------------------------------ */
/* Settings load callback                                               */
/* ------------------------------------------------------------------ */

static int settings_load_cb(const char *key, size_t len,
                             settings_read_cb read_cb, void *cb_arg,
                             void *param)
{
    ARG_UNUSED(param);

    if (strcmp(key, "brightness") == 0 && len == sizeof(int)) {
        int val;
        if (read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
            lv_slider_set_value(g_brightness_slider, val, LV_ANIM_OFF);
            lv_label_set_text_fmt(g_brightness_val_label, "%d", val);
        }
    } else if (strcmp(key, "timeout_s") == 0 && len == sizeof(int32_t)) {
        int32_t val;
        if (read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
            lv_spinbox_set_value(g_timeout_spinbox, val);
        }
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(akira_display_sh, "akira/display",
                                NULL, settings_load_cb, NULL, NULL);

/* ------------------------------------------------------------------ */
/* Screen construction                                                  */
/* ------------------------------------------------------------------ */

static void add_row_label(lv_obj_t *parent, const char *text, int y_ofs)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_add_style(lbl, &g_style_list_item, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 8, y_ofs);
}

static void build_screen(void)
{
    g_screen = lv_obj_create(NULL);
    lv_obj_add_style(g_screen, &g_style_screen, 0);

    shell_theme_make_header(g_screen, "Display");
    shell_theme_make_footer(g_screen, "B:Back", "");

    int y = SHELL_HEADER_H + 8;

    /* --- Brightness --- */
    add_row_label(g_screen, "Brightness", y);
    y += 20;

    g_brightness_slider = lv_slider_create(g_screen);
    lv_slider_set_range(g_brightness_slider, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
    lv_slider_set_value(g_brightness_slider, BRIGHTNESS_MAX, LV_ANIM_OFF);
    lv_obj_set_size(g_brightness_slider, SHELL_SCREEN_W - 72, 16);
    lv_obj_align(g_brightness_slider, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_add_event_cb(g_brightness_slider, brightness_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    g_brightness_val_label = lv_label_create(g_screen);
    lv_label_set_text(g_brightness_val_label, "255");
    lv_obj_add_style(g_brightness_val_label, &g_style_list_item, 0);
    lv_obj_align_to(g_brightness_val_label, g_brightness_slider,
                    LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    y += 28;

    /* Separator */
    lv_obj_t *sep = lv_obj_create(g_screen);
    lv_obj_set_size(sep, SHELL_SCREEN_W - 16, 1);
    lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_add_style(sep, &g_style_separator, 0);
    y += 8;

    /* --- Screen-off timeout --- */
    add_row_label(g_screen, "Screen-off timeout (s)", y);
    y += 20;

    g_timeout_spinbox = lv_spinbox_create(g_screen);
    lv_spinbox_set_range(g_timeout_spinbox, TIMEOUT_MIN_S, TIMEOUT_MAX_S);
    lv_spinbox_set_value(g_timeout_spinbox, 60);
    lv_spinbox_set_digit_format(g_timeout_spinbox, 3, 0);
    lv_obj_set_width(g_timeout_spinbox, 80);
    lv_obj_align(g_timeout_spinbox, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_add_event_cb(g_timeout_spinbox, timeout_save_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_add_event_cb(g_screen, back_event_cb, LV_EVENT_KEY, NULL);

    /* Load persisted values */
    settings_load_subtree("akira/display");
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void display_screen_load(void)
{
    if (!g_screen) {
        build_screen();
    }
    lv_scr_load(g_screen);
}
