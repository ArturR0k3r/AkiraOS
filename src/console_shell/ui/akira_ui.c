/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file akira_ui.c
 * @brief AkiraConsole shared UI kit — native renderer over akira_display_*.
 *
 * Four primitives, escalating pixel weight, zero colour:
 *   outline < solid fill < dither < 3px heavy stroke.
 * The 3px stroke is reserved for akira_ui_confirm_dialog() so a user knows a
 * guarded syscall is being gated without reading the text.
 */

#include "akira_ui.h"

#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <api/akira_display_api.h>
#include <api/akira_input_api.h>

/* ---- helpers ---------------------------------------------------------- */

static inline int text_w(const char *s) { return (int)strlen(s) * AKIRA_UI_CELL_W; }

/* 50% checkerboard fill of an arbitrary rect (primitive #3). */
static void dither_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (((i + j) & 1) == 0) {
                akira_display_pixel(x + i, y + j, color);
            }
        }
    }
}

/* ---- icon blit -------------------------------------------------------- */

void akira_ui_icon_1bpp(int x, int y, int scale, const uint32_t *icon,
                        int w, int rows, uint16_t fg, uint16_t bg,
                        akira_ui_fill_t fill)
{
    if (!icon || scale < 1) {
        return;
    }
    for (int r = 0; r < rows; r++) {
        uint32_t bits = icon[r];
        for (int c = 0; c < w; c++) {
            bool set = (bits & (1u << (31 - c))) != 0;
            int px = x + c * scale;
            int py = y + r * scale;

            switch (fill) {
            case AKIRA_UI_SOLID:
                akira_display_rect(px, py, scale, scale, set ? fg : bg);
                break;
            case AKIRA_UI_TRANSPARENT:
                if (set) {
                    akira_display_rect(px, py, scale, scale, fg);
                }
                break;
            case AKIRA_UI_DITHER:
                /* set pixels only on the checker → "greyed" 1-bit look */
                if (set && ((c + r) & 1) == 0) {
                    akira_display_rect(px, py, scale, scale, fg);
                } else {
                    akira_display_rect(px, py, scale, scale, bg);
                }
                break;
            }
        }
    }
}

/* ---- status bar ------------------------------------------------------- */

void akira_ui_status_bar(const akira_ui_status_t *s)
{
    int W, H;
    akira_display_get_size(&W, &H);
    const int bar_h = AKIRA_UI_STATUSBAR_H;
    const int txt_y = (bar_h - 10) / 2;

    /* The one inverted bar: solid PAPER fill, everything else drawn in INK. */
    akira_display_rect(0, 0, W, bar_h, AKIRA_UI_PAPER);

    /* --- left slot: title, else battery gauge --- */
    if (s->title && s->title[0]) {
        akira_display_text(6, txt_y, s->title, AKIRA_UI_INK);
    } else if (s->battery_pct >= 0) {
        int by = txt_y;
        akira_display_rect_outline(4, by, 22, 10, AKIRA_UI_INK);
        akira_display_rect(26, by + 3, 2, 4, AKIRA_UI_INK); /* nub */
        int pct = s->battery_pct > 100 ? 100 : s->battery_pct;
        int segs = (pct * 5 + 50) / 100;
        for (int i = 0; i < 5; i++) {
            akira_display_rect(6 + i * 4, by + 2, 3, 6,
                               (i < segs) ? AKIRA_UI_INK : AKIRA_UI_PAPER);
        }
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        akira_display_text(30, txt_y, buf, AKIRA_UI_INK);
    }

    /* --- right slot: clock, then wifi/bt cluster to its left --- */
    int right = W - 4;
    if (s->clock && s->clock[0]) {
        akira_display_text(W - text_w(s->clock) - 4, txt_y, s->clock, AKIRA_UI_INK);
        right = W - text_w(s->clock) - 8;
    }

    int icon_y = (bar_h - 16) / 2;
    if (s->show_bt && s->icon_bt) {
        right -= 20;
        akira_ui_icon_1bpp(right, icon_y, 1, s->icon_bt, 20, 16,
                           AKIRA_UI_INK, AKIRA_UI_PAPER,
                           s->bt_on ? AKIRA_UI_TRANSPARENT : AKIRA_UI_DITHER);
    }
    if (s->show_wifi && s->icon_wifi) {
        right -= 20;
        akira_ui_icon_1bpp(right, icon_y, 1, s->icon_wifi, 20, 16,
                           AKIRA_UI_INK, AKIRA_UI_PAPER,
                           s->wifi_on ? AKIRA_UI_TRANSPARENT : AKIRA_UI_DITHER);
    }
}

/* ---- list row --------------------------------------------------------- */

void akira_ui_list_row(int y, int h, const char *text, const char *meta,
                       int meter_0_5, bool selected)
{
    int W, H;
    akira_display_get_size(&W, &H);
    uint16_t bg = selected ? AKIRA_UI_PAPER : AKIRA_UI_INK;
    uint16_t fg = selected ? AKIRA_UI_INK : AKIRA_UI_PAPER;
    int ty = y + (h - 10) / 2;

    akira_display_rect(0, y, W, h, bg);
    if (text) {
        akira_display_text(6, ty, text, fg);
    }

    int rx = W - 6;
    if (meter_0_5 >= 0) {
        const int nblk = 5, bw = 6, bh = 8, gap = 2;
        int mx = W - 6 - nblk * (bw + gap);
        int my = y + (h - bh) / 2;
        for (int i = 0; i < nblk; i++) {
            int cx = mx + i * (bw + gap);
            if (i < meter_0_5) {
                akira_display_rect(cx, my, bw, bh, fg);
            } else {
                akira_display_rect_outline(cx, my, bw, bh, fg);
            }
        }
        rx = mx - 6;
    }
    if (meta && meta[0]) {
        akira_display_text(rx - text_w(meta), ty, meta, fg);
    }
}

/* ---- grid tile -------------------------------------------------------- */

void akira_ui_grid_tile(int x, int y, int w, int h, int scale,
                        const uint32_t *icon, int icon_w, int icon_rows,
                        const char *label, bool selected)
{
    uint16_t bg = selected ? AKIRA_UI_PAPER : AKIRA_UI_INK;
    uint16_t fg = selected ? AKIRA_UI_INK : AKIRA_UI_PAPER;

    akira_display_rect(x, y, w, h, bg);
    if (!selected) {
        akira_display_rect_outline(x, y, w, h, fg);
    }
    if (icon) {
        int sw = icon_w * scale;
        akira_ui_icon_1bpp(x + (w - sw) / 2, y + 8, scale, icon,
                           icon_w, icon_rows, fg, bg, AKIRA_UI_TRANSPARENT);
    }
    if (label && label[0]) {
        akira_display_text(x + (w - text_w(label)) / 2, y + h - 14, label, fg);
    }
}

/* ---- button ----------------------------------------------------------- */

void akira_ui_button(int x, int y, int w, int h, const char *label, bool pressed)
{
    uint16_t bg = pressed ? AKIRA_UI_PAPER : AKIRA_UI_INK;
    uint16_t fg = pressed ? AKIRA_UI_INK : AKIRA_UI_PAPER;

    akira_display_rect(x, y, w, h, bg);
    if (!pressed) {
        akira_display_rect_outline(x, y, w, h, fg);
    }
    if (label) {
        akira_display_text(x + (w - text_w(label)) / 2, y + (h - 10) / 2, label, fg);
    }
}

/* ---- toggle ----------------------------------------------------------- */

void akira_ui_toggle(int x, int y, const char *label, bool on)
{
    if (label) {
        akira_display_text(x, y + 2, label, AKIRA_UI_PAPER);
    }
    int tx = x + (label ? text_w(label) + 8 : 0);
    const int tw = 26, th = 14;

    if (on) {
        akira_display_rect(tx, y, tw, th, AKIRA_UI_PAPER);
        akira_display_rect(tx + tw - 12, y + 2, 10, 10, AKIRA_UI_INK); /* knob R */
    } else {
        akira_display_rect_outline(tx, y, tw, th, AKIRA_UI_PAPER);
        akira_display_rect(tx + 2, y + 2, 10, 10, AKIRA_UI_PAPER);     /* knob L */
    }
}

/* ---- meter ------------------------------------------------------------ */

void akira_ui_meter(int x, int y, int w, int h, int value, int max)
{
    /* outline track + proportional fill */
    akira_display_progress_bar(x, y, w, h, value, max, AKIRA_UI_PAPER, AKIRA_UI_INK);
}

/* ---- tag -------------------------------------------------------------- */

void akira_ui_tag(int x, int y, const char *text, bool dithered)
{
    int w = text_w(text) + 8, h = 14;
    if (dithered) {
        dither_rect(x, y, w, h, AKIRA_UI_PAPER);
        akira_display_rect_outline(x, y, w, h, AKIRA_UI_PAPER);
        akira_display_text(x + 4, y + 2, text, AKIRA_UI_INK);
    } else {
        akira_display_rect_outline(x, y, w, h, AKIRA_UI_PAPER);
        akira_display_text(x + 4, y + 2, text, AKIRA_UI_PAPER);
    }
}

/* ---- alert ------------------------------------------------------------ */

void akira_ui_alert(const char *title, const char *subtitle)
{
    int W, H;
    akira_display_get_size(&W, &H);
    int cx = W / 2;

    akira_display_clear(AKIRA_UI_INK);

    /* alert triangle + exclamation */
    int apex_y = H / 2 - 54;
    int ts = 22;
    akira_display_triangle(cx, apex_y, cx - ts, apex_y + ts * 2,
                           cx + ts, apex_y + ts * 2, AKIRA_UI_PAPER);
    akira_display_vline(cx, apex_y + 12, ts - 6, AKIRA_UI_PAPER);
    akira_display_rect(cx - 1, apex_y + ts + 4, 2, 2, AKIRA_UI_PAPER);

    if (title && title[0]) {
        akira_display_text(cx - text_w(title) / 2, H / 2 + 4, title, AKIRA_UI_PAPER);
    }
    if (subtitle && subtitle[0]) {
        akira_display_text(cx - text_w(subtitle) / 2, H / 2 + 20, subtitle,
                           AKIRA_UI_PAPER);
    }
    akira_display_flush();
}

/* ---- confirm dialog (the only 3px element) ---------------------------- */

static void confirm_render(int bx, int by, int bw, int bh,
                           const char *capability_name, const char *question,
                           int sel)
{
    int cx = bx + bw / 2;

    akira_display_clear(AKIRA_UI_INK);

    /* Heavy 3px stroke — reserved marker for a Capability-Guard gate. */
    for (int i = 0; i < 3; i++) {
        akira_display_rect_outline(bx - i, by - i, bw + 2 * i, bh + 2 * i,
                                   AKIRA_UI_PAPER);
    }

    /* Capability glyph: a small shield-ish outline triangle. */
    int gy = by + 14, gs = 12;
    akira_display_triangle(cx, gy, cx - gs, gy + gs, cx + gs, gy + gs,
                           AKIRA_UI_PAPER);
    akira_display_vline(cx, gy + 4, gs - 4, AKIRA_UI_PAPER);

    if (question && question[0]) {
        akira_display_text(cx - text_w(question) / 2, by + 44, question,
                           AKIRA_UI_PAPER);
    }

    char cap[64];
    snprintf(cap, sizeof(cap), "capability: %s - restricted",
             capability_name ? capability_name : "?");
    akira_display_text(cx - text_w(cap) / 2, by + 64, cap, AKIRA_UI_PAPER);

    /* cancel = plain text (focus = outline box); confirm = solid fill. */
    int btn_y = by + bh - 42;
    const char *cancel = "cancel";
    int cxl = bx + bw / 4 - text_w(cancel) / 2;
    if (sel == 0) {
        akira_display_rect_outline(cxl - 8, btn_y - 4, text_w(cancel) + 16, 26,
                                   AKIRA_UI_PAPER);
    }
    akira_display_text(cxl, btn_y + 4, cancel, AKIRA_UI_PAPER);

    akira_ui_button(bx + bw / 2 + 8, btn_y, bw / 2 - 24, 26, "confirm", sel == 1);

    akira_display_flush();
}

bool akira_ui_confirm_dialog(const char *capability_name, const char *question)
{
    int W, H;
    akira_display_get_size(&W, &H);
    int bw = W - 80, bh = 160;
    int bx = (W - bw) / 2, by = (H - bh) / 2;

    int sel = 0; /* default to cancel — safest for a guarded action */
    confirm_render(bx, by, bw, bh, capability_name, question, sel);

    uint32_t prev = akira_input_get_bitmask();
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t now = akira_input_get_bitmask();
        uint32_t pressed = now & ~prev; /* rising edges only */
        prev = now;

        if (pressed & (BIT(AKIRA_BTN_LEFT) | BIT(AKIRA_BTN_RIGHT))) {
            sel ^= 1;
            confirm_render(bx, by, bw, bh, capability_name, question, sel);
        }
        if (pressed & BIT(AKIRA_BTN_A)) {
            return sel == 1;
        }
        if (pressed & BIT(AKIRA_BTN_B)) {
            return false;
        }
    }
}
