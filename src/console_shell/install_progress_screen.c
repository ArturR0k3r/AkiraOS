/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_install_progress
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_install_progress, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file install_progress_screen.c
 * @brief Modal install progress overlay — Liquid Crystal style.
 *
 * Centred 288x110 panel with:
 *   - Dither glass backdrop
 *   - App name label
 *   - Segmented progress bar (20 segments, 1 px gap)
 *   - Status message
 *
 * Pure akira_display_* — no LVGL.
 */

#include "install_progress_screen.h"

#include <api/akira_display_api.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>


#include "shell_theme.h"

#define OVL_W    288
#define OVL_H    110
#define OVL_X    ((SCR_W - OVL_W) / 2)
#define OVL_Y    ((SCR_H - OVL_H) / 2)

/* ------------------------------------------------------------------ */



static void draw_centred(int x, int y, int w, const char *s, uint16_t fg, uint16_t bg)
{
    int tw = (int)strlen(s) * 8;
    int lx = x + (tw < w ? (w - tw) / 2 : 0);
    akira_display_rect(x, y, w, 10, bg);
    akira_display_text(lx, y, s, fg);
}

/* ------------------------------------------------------------------ */

void install_progress_show(const char *name, int pct, const char *msg)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    /* Panel */
    akira_display_rect(OVL_X, OVL_Y, OVL_W, OVL_H, C_BLACK);
    akira_display_rect_outline(OVL_X,     OVL_Y,     OVL_W,     OVL_H,     C_WHITE);
    akira_display_rect_outline(OVL_X + 1, OVL_Y + 1, OVL_W - 2, OVL_H - 2, C_WHITE);

    /* Title */
    draw_centred(OVL_X + 4, OVL_Y + 6, OVL_W - 8, "INSTALLING", C_WHITE, C_BLACK);
    akira_display_hline(OVL_X + 4, OVL_Y + 18, OVL_W - 8, C_WHITE);

    /* App name */
    const char *disp_name = name ? name : "app";
    draw_centred(OVL_X + 4, OVL_Y + 24, OVL_W - 8, disp_name, C_WHITE, C_BLACK);

    /* Segmented progress bar: 20 segments across OVL_W-16 */
    int bar_x = OVL_X + 8;
    int bar_y = OVL_Y + 40;
    int bar_w = OVL_W - 16;
    int bar_h = 14;
    int segs  = 20;
    int sw    = (bar_w - (segs - 1)) / segs;   /* segment width */

    akira_display_rect_outline(bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, C_WHITE);
    int filled = pct * segs / 100;
    for (int i = 0; i < segs; i++) {
        int sx  = bar_x + i * (sw + 1);
        uint16_t sc = (i < filled) ? C_WHITE : C_DKGRAY;
        akira_display_rect(sx, bar_y, sw, bar_h, sc);
    }

    /* Percentage */
    char pct_str[8];
    snprintf(pct_str, sizeof(pct_str), "%d%%", pct);
    draw_centred(OVL_X + 4, OVL_Y + 58, OVL_W - 8, pct_str, C_WHITE, C_BLACK);

    /* Status message */
    if (msg && *msg) {
        draw_centred(OVL_X + 4, OVL_Y + 74, OVL_W - 8, msg, C_GRAY, C_BLACK);
    }

    akira_display_flush();

    LOG_DBG("Install progress: '%s' %d%% — %s",
            disp_name, pct, msg ? msg : "");
}

void install_progress_hide(void)
{
    /* Nothing to destroy — next full redraw will overwrite */
    LOG_DBG("Install progress overlay hidden");
}
