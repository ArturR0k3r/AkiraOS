/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_ota_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_ota_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file ota_screen.c
 * @brief OTA firmware update: version display, check and progress bar.
 */

#include <zephyr/kernel.h>


#include "../shell_theme.h"
#include "ota_screen.h"

#if defined(CONFIG_AKIRA_OTA)
#include "../../connectivity/ota/ota_manager.h"
#endif

static lv_obj_t *g_screen;
static lv_obj_t *g_status_label;
static lv_obj_t *g_progress_bar;
static lv_obj_t *g_check_btn;

/* ------------------------------------------------------------------ */
/* Check / apply button                                                 */
/* ------------------------------------------------------------------ */

static void check_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    lv_label_set_text(g_status_label, "Checking for update…");
    lv_bar_set_value(g_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_state(g_check_btn, LV_STATE_DISABLED, true);
    lv_timer_handler();

#if defined(CONFIG_AKIRA_OTA)
    /* OTA is driven by the web-server transport; the screen just shows
     * live progress via the registered callback. */
    lv_label_set_text(g_status_label, "OTA via web upload only");
#else
    lv_label_set_text(g_status_label, "OTA not enabled");
#endif

    lv_obj_set_state(g_check_btn, LV_STATE_DISABLED, false);
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
/* Screen construction                                                  */
/* ------------------------------------------------------------------ */

static void build_screen(void)
{
    g_screen = lv_obj_create(NULL);
    lv_obj_add_style(g_screen, &g_style_screen, 0);

    shell_theme_make_header(g_screen, "Firmware Update");
    shell_theme_make_footer(g_screen, "B:Back", "A:Check");

    int y = SHELL_HEADER_H + 10;

    /* Current version */
    lv_obj_t *ver_lbl = lv_label_create(g_screen);
    lv_label_set_text_fmt(ver_lbl, "Version: %s",
                          CONFIG_AKIRA_OS_VERSION);
    lv_obj_set_style_text_font(ver_lbl, SHELL_FONT_SMALL, 0);
    lv_obj_align(ver_lbl, LV_ALIGN_TOP_LEFT, 8, y);
    y += 24;

    /* Status label */
    g_status_label = lv_label_create(g_screen);
    lv_label_set_text(g_status_label, "Press A to check for updates");
    lv_obj_set_style_text_font(g_status_label, SHELL_FONT_SMALL, 0);
    lv_obj_set_style_text_color(g_status_label,
                                lv_color_make(0x50, 0x50, 0x50), 0);
    lv_obj_set_width(g_status_label, SHELL_SCREEN_W - 16);
    lv_obj_align(g_status_label, LV_ALIGN_TOP_LEFT, 8, y);
    y += 24;

    /* Progress bar */
    g_progress_bar = lv_bar_create(g_screen);
    lv_obj_set_size(g_progress_bar, SHELL_SCREEN_W - 16, 16);
    lv_bar_set_range(g_progress_bar, 0, 100);
    lv_bar_set_value(g_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_align(g_progress_bar, LV_ALIGN_TOP_LEFT, 8, y);
    /* White indicator on dark bar */
    lv_obj_set_style_bg_color(g_progress_bar,
                              lv_color_make(0x30, 0x30, 0x30), 0);
    lv_obj_set_style_bg_color(g_progress_bar,
                              lv_color_white(),
                              LV_PART_INDICATOR);
    y += 28;

    /* Check button */
    g_check_btn = lv_btn_create(g_screen);
    lv_obj_set_size(g_check_btn, 160, 36);
    lv_obj_align(g_check_btn, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_add_event_cb(g_check_btn, check_btn_cb,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(g_check_btn);
    lv_label_set_text(btn_lbl, "Check Update");
    lv_obj_center(btn_lbl);

    lv_obj_add_event_cb(g_screen, back_event_cb, LV_EVENT_KEY, NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void ota_screen_load(void)
{
    if (!g_screen) {
        build_screen();
    }
    lv_scr_load(g_screen);
}
