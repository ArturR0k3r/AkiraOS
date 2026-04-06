/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_install_progress
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_install_progress, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file install_progress_screen.c
 * @brief Modal install progress overlay reused by SD, BLE, and HTTP installers.
 *
 * A semi-transparent black overlay (200×110 px, centred) containing:
 *   - App name label
 *   - Progress bar (0..100 %)
 *   - Status message label
 *
 * The overlay is built lazily on first show() call and reused.
 * Destroyed on hide().
 */

#include "install_progress_screen.h"
#include "shell_theme.h"


#include <string.h>
#include <zephyr/kernel.h>

#define OVERLAY_W 220
#define OVERLAY_H 110

static lv_obj_t *g_overlay;
static lv_obj_t *g_lbl_name;
static lv_obj_t *g_bar;
static lv_obj_t *g_lbl_msg;

/* ------------------------------------------------------------------ */

static void overlay_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* Dimmed backdrop */
    g_overlay = lv_obj_create(scr);
    lv_obj_set_size(g_overlay, OVERLAY_W, OVERLAY_H);
    lv_obj_center(g_overlay);
    lv_obj_set_style_bg_color(g_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_color(g_overlay,
                                  lv_color_white(), 0);
    lv_obj_set_style_border_width(g_overlay, 1, 0);
    lv_obj_set_style_radius(g_overlay, 8, 0);
    lv_obj_set_style_pad_all(g_overlay, 10, 0);
    lv_obj_set_scrollbar_mode(g_overlay, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(g_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_overlay,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(g_overlay, 6, 0);

    /* App name */
    g_lbl_name = lv_label_create(g_overlay);
    lv_label_set_text(g_lbl_name, "Installing...");
    lv_obj_set_style_text_color(g_lbl_name, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_lbl_name, SHELL_FONT_SMALL, 0);

    /* Progress bar */
    g_bar = lv_bar_create(g_overlay);
    lv_obj_set_size(g_bar, OVERLAY_W - 30, 14);
    lv_bar_set_range(g_bar, 0, 100);
    lv_bar_set_value(g_bar, 0, LV_ANIM_ON);
    lv_obj_set_style_bg_color(g_bar, lv_color_make(60, 60, 60), 0);
    lv_obj_set_style_bg_color(g_bar, lv_color_white(),
                              LV_PART_INDICATOR);

    /* Status message */
    g_lbl_msg = lv_label_create(g_overlay);
    lv_label_set_text(g_lbl_msg, "");
    lv_obj_set_style_text_color(g_lbl_msg, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_text_font(g_lbl_msg, SHELL_FONT_SMALL, 0);
}

/* ------------------------------------------------------------------ */

void install_progress_show(const char *name, int pct, const char *msg)
{
    if (!g_overlay) {
        overlay_create();
    }

    if (g_lbl_name) {
        lv_label_set_text(g_lbl_name, name ? name : "Installing...");
    }

    if (g_bar) {
        int clamped = (pct < 0) ? 0 : (pct > 100) ? 100 : pct;
        lv_bar_set_value(g_bar, clamped, LV_ANIM_ON);
    }

    if (g_lbl_msg && msg) {
        lv_label_set_text(g_lbl_msg, msg);
    }

    lv_timer_handler();
    LOG_DBG("Install progress: '%s' %d%% — %s",
            name ? name : "?", pct, msg ? msg : "");
}

void install_progress_hide(void)
{
    if (g_overlay) {
        lv_obj_del(g_overlay);
        g_overlay  = NULL;
        g_lbl_name = NULL;
        g_bar      = NULL;
        g_lbl_msg  = NULL;
    }
}

#endif /* CONFIG_LVGL */
