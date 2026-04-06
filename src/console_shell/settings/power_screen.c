/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_power_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_power_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file power_screen.c
 * @brief Idle sleep and display-off timeout configuration.
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>


#include "../shell_theme.h"
#include "power_screen.h"

#define SLEEP_MIN_S        30
#define SLEEP_MAX_S        3600
#define DISPLAY_OFF_MIN_S  5
#define DISPLAY_OFF_MAX_S  600

static lv_obj_t *g_screen;
static lv_obj_t *g_sleep_spinbox;
static lv_obj_t *g_dispoff_spinbox;

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
/* Save callbacks                                                       */
/* ------------------------------------------------------------------ */

static void sleep_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    int32_t val = lv_spinbox_get_value(g_sleep_spinbox);
    settings_save_one("akira/power/sleep_s", &val, sizeof(val));
    LOG_DBG("Sleep timeout set to %d s", val);
}

static void dispoff_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    int32_t val = lv_spinbox_get_value(g_dispoff_spinbox);
    settings_save_one("akira/power/display_off_s", &val, sizeof(val));
    LOG_DBG("Display-off timeout set to %d s", val);
}

/* ------------------------------------------------------------------ */
/* Settings handler                                                     */
/* ------------------------------------------------------------------ */

static int settings_load_cb(const char *key, size_t len,
                             settings_read_cb read_cb, void *cb_arg,
                             void *param)
{
    ARG_UNUSED(param);

    if (strcmp(key, "sleep_s") == 0 && len == sizeof(int32_t)) {
        int32_t val;
        if (read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
            lv_spinbox_set_value(g_sleep_spinbox, val);
        }
    } else if (strcmp(key, "display_off_s") == 0 &&
               len == sizeof(int32_t)) {
        int32_t val;
        if (read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
            lv_spinbox_set_value(g_dispoff_spinbox, val);
        }
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(akira_power_sh, "akira/power",
                                NULL, settings_load_cb, NULL, NULL);

/* ------------------------------------------------------------------ */
/* Screen construction helpers                                          */
/* ------------------------------------------------------------------ */

static lv_obj_t *make_spinbox_row(lv_obj_t *parent, const char *text,
                                   int min_val, int max_val,
                                   int default_val, int y,
                                   lv_event_cb_t cb)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_add_style(lbl, &g_style_list_item, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 8, y);

    lv_obj_t *sb = lv_spinbox_create(parent);
    lv_spinbox_set_range(sb, min_val, max_val);
    lv_spinbox_set_value(sb, default_val);
    lv_spinbox_set_digit_format(sb, 4, 0);
    lv_obj_set_width(sb, 90);
    lv_obj_align(sb, LV_ALIGN_TOP_RIGHT, -8, y);
    lv_obj_add_event_cb(sb, cb, LV_EVENT_VALUE_CHANGED, NULL);

    return sb;
}

static void build_screen(void)
{
    g_screen = lv_obj_create(NULL);
    lv_obj_add_style(g_screen, &g_style_screen, 0);

    shell_theme_make_header(g_screen, "Power");
    shell_theme_make_footer(g_screen, "B:Back", "");

    int y = SHELL_HEADER_H + 12;

    g_sleep_spinbox = make_spinbox_row(g_screen,
        "Idle sleep (s)", SLEEP_MIN_S, SLEEP_MAX_S, 300, y, sleep_cb);
    y += 40;

    /* Separator */
    lv_obj_t *sep = lv_obj_create(g_screen);
    lv_obj_set_size(sep, SHELL_SCREEN_W - 16, 1);
    lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_add_style(sep, &g_style_separator, 0);
    y += 10;

    g_dispoff_spinbox = make_spinbox_row(g_screen,
        "Display-off (s)", DISPLAY_OFF_MIN_S, DISPLAY_OFF_MAX_S,
        60, y, dispoff_cb);

    lv_obj_add_event_cb(g_screen, back_event_cb, LV_EVENT_KEY, NULL);

    settings_load_subtree("akira/power");
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void power_screen_load(void)
{
    if (!g_screen) {
        build_screen();
    }
    lv_scr_load(g_screen);
}
