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

    /* Rounded pill near the top (ink interior + paper outline, content paper). */
    int pill_h = bar_h - 2, pr = pill_h / 2;
    akira_display_rounded_rect_fill(2, 1, W - 4, pill_h, pr, AKIRA_UI_INK);
    akira_display_rounded_rect(2, 1, W - 4, pill_h, pr, AKIRA_UI_PAPER);

    /* --- left slot: title, else battery gauge --- */
    if (s->title && s->title[0]) {
        akira_display_text(10, txt_y, s->title, AKIRA_UI_PAPER);
    } else if (s->battery_pct >= 0) {
        int bx = 8, by = txt_y;
        akira_display_rounded_rect(bx, by, 22, 10, 2, AKIRA_UI_PAPER);
        akira_display_rect(bx + 22, by + 3, 2, 4, AKIRA_UI_PAPER); /* nub */
        int pct = s->battery_pct > 100 ? 100 : s->battery_pct;
        int segs = (pct * 5 + 50) / 100;
        for (int i = 0; i < 5; i++) {
            akira_display_rect(bx + 2 + i * 4, by + 2, 3, 6,
                               (i < segs) ? AKIRA_UI_PAPER : AKIRA_UI_INK);
        }
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        akira_display_text(bx + 28, txt_y, buf, AKIRA_UI_PAPER);
    }

    /* --- right slot: clock, then wifi/bt cluster to its left --- */
    int right = W - 8;
    if (s->clock && s->clock[0]) {
        akira_display_text(W - text_w(s->clock) - 10, txt_y, s->clock, AKIRA_UI_PAPER);
        right = W - text_w(s->clock) - 14;
    }

    int icon_y = (bar_h - 16) / 2;
    if (s->show_bt && s->icon_bt) {
        right -= 22;
        akira_ui_icon_1bpp(right, icon_y, 1, s->icon_bt, 20, 16,
                           AKIRA_UI_PAPER, AKIRA_UI_INK,
                           s->bt_on ? AKIRA_UI_TRANSPARENT : AKIRA_UI_DITHER);
    }
    if (s->show_wifi && s->icon_wifi) {
        right -= 22;
        akira_ui_icon_1bpp(right, icon_y, 1, s->icon_wifi, 20, 16,
                           AKIRA_UI_PAPER, AKIRA_UI_INK,
                           s->wifi_on ? AKIRA_UI_TRANSPARENT : AKIRA_UI_DITHER);
    }
}

/* ---- list row --------------------------------------------------------- */

void akira_ui_list_row(int y, int h, const char *text, const char *meta,
                       int meter_0_5, bool selected)
{
    int W, H;
    akira_display_get_size(&W, &H);
    uint16_t fg = selected ? AKIRA_UI_INK : AKIRA_UI_PAPER;

    /* Each row is its own rounded dither-shadow card with side margins. */
    const int mx = 6;
    int cw = W - mx * 2;
    int rh = h - 4; /* leave the bottom gap for the dither shadow */
    akira_ui_dither_card(mx, y, cw, rh, 8, selected, selected ? 3 : 2);

    int ty = y + (rh - 10) / 2;
    if (text) {
        akira_display_text(mx + 10, ty, text, fg);
    }

    int rx = mx + cw - 10;
    if (meter_0_5 >= 0) {
        const int nblk = 5, bw = 6, bh = 8, gap = 2;
        int bx0 = mx + cw - 10 - nblk * (bw + gap);
        int my = y + (rh - bh) / 2;
        for (int i = 0; i < nblk; i++) {
            int cx = bx0 + i * (bw + gap);
            if (i < meter_0_5) {
                akira_display_rect(cx, my, bw, bh, fg);
            } else {
                akira_display_rect_outline(cx, my, bw, bh, fg);
            }
        }
        rx = bx0 - 6;
    }
    if (meta && meta[0]) {
        akira_display_text(rx - text_w(meta), ty, meta, fg);
    }
}

/* ---- dither-shadow card (signature elevation primitive) --------------- */

/*
 * 50% checkerboard fill clipped to a rounded-rect mask — the "shadow". There
 * is no pattern-fill primitive on-device, so it is composed from pixels. The
 * 2x2 checker is pure parity ((i+j)&1) — no lookup cache buys anything; the
 * per-corner distance test is the only real cost. If profiling shows redraw
 * lag on card-dense screens, pre-render this once into a display_bitmap and
 * blit instead (see akira_ui_dither_card notes).
 */
static void dither_rounded(int x, int y, int w, int h, int r, uint16_t color)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int cx = -1, cy = -1; /* offset into the nearest corner quadrant */
            if (i < r && j < r)                { cx = r - 1 - i; cy = r - 1 - j; }
            else if (i >= w - r && j < r)      { cx = i - (w - r); cy = r - 1 - j; }
            else if (i < r && j >= h - r)      { cx = r - 1 - i; cy = j - (h - r); }
            else if (i >= w - r && j >= h - r) { cx = i - (w - r); cy = j - (h - r); }
            if (cx >= 0 && cx * cx + cy * cy > r * r) {
                continue; /* pixel lies outside the rounded corner */
            }
            if (((i + j) & 1) == 0) {
                akira_display_pixel(x + i, y + j, color);
            }
        }
    }
}

void akira_ui_dither_card(int x, int y, int w, int h, int radius,
                          bool selected, int shadow_offset)
{
    /* 1. Elevation shadow: dithered rounded silhouette, offset down-right. */
    if (shadow_offset > 0) {
        dither_rounded(x + shadow_offset, y + shadow_offset, w, h, radius,
                       AKIRA_UI_PAPER);
    }
    /* 2. Card body: a solid fill erases the shadow beneath it. Selected =
     *    paper-on-ink invert; idle = ink body with a paper outline. */
    akira_display_rounded_rect_fill(x, y, w, h, radius,
                                    selected ? AKIRA_UI_PAPER : AKIRA_UI_INK);
    if (!selected) {
        akira_display_rounded_rect(x, y, w, h, radius, AKIRA_UI_PAPER);
    }
}

/* ---- grid tile -------------------------------------------------------- */

void akira_ui_grid_tile(int x, int y, int w, int h, int scale,
                        const uint32_t *icon, int icon_w, int icon_rows,
                        const char *label, bool selected)
{
    uint16_t bg = selected ? AKIRA_UI_PAPER : AKIRA_UI_INK;
    uint16_t fg = selected ? AKIRA_UI_INK : AKIRA_UI_PAPER;

    /* Playdate-style: rounded dither-shadow card instead of a square outline. */
    akira_ui_dither_card(x, y, w, h, /*radius=*/10, selected, /*shadow=*/4);

    if (icon) {
        int sw = icon_w * scale;
        akira_ui_icon_1bpp(x + (w - sw) / 2, y + 12, scale, icon,
                           icon_w, icon_rows, fg, bg, AKIRA_UI_TRANSPARENT);
    }
    if (label && label[0]) {
        akira_display_text(x + (w - text_w(label)) / 2, y + h - 16, label, fg);
    }
}

/* ---- button ----------------------------------------------------------- */

void akira_ui_button(int x, int y, int w, int h, const char *label, bool pressed)
{
    uint16_t fg = pressed ? AKIRA_UI_INK : AKIRA_UI_PAPER;

    /* Rounded dither-shadow card; pressed = filled invert + more elevation. */
    akira_ui_dither_card(x, y, w, h, 8, pressed, pressed ? 3 : 2);
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
    const int tw = 28, th = 16, r = 8;

    if (on) {
        akira_display_rounded_rect_fill(tx, y, tw, th, r, AKIRA_UI_PAPER);
        akira_display_circle_fill(tx + tw - 8, y + th / 2, 5, AKIRA_UI_INK);  /* knob R */
    } else {
        akira_display_rounded_rect(tx, y, tw, th, r, AKIRA_UI_PAPER);
        akira_display_circle_fill(tx + 8, y + th / 2, 5, AKIRA_UI_PAPER);     /* knob L */
    }
}

/* ---- meter ------------------------------------------------------------ */

void akira_ui_meter(int x, int y, int w, int h, int value, int max)
{
    if (max <= 0) max = 1;
    if (value < 0) value = 0;
    if (value > max) value = max;
    int r = h / 2;
    akira_display_rounded_rect(x, y, w, h, r, AKIRA_UI_PAPER);        /* pill track */
    int fw = (w - 4) * value / max;
    if (fw > 0) {
        akira_display_rounded_rect_fill(x + 2, y + 2, fw, h - 4, r > 2 ? r - 2 : 0,
                                        AKIRA_UI_PAPER);
    }
}

/* ---- tag -------------------------------------------------------------- */

void akira_ui_tag(int x, int y, const char *text, bool dithered)
{
    int w = text_w(text) + 10, h = 16, r = 7;
    if (dithered) {
        dither_rounded(x, y, w, h, r, AKIRA_UI_PAPER);
        akira_display_rounded_rect(x, y, w, h, r, AKIRA_UI_PAPER);
        akira_display_text(x + 5, y + 3, text, AKIRA_UI_INK);
    } else {
        akira_display_rounded_rect(x, y, w, h, r, AKIRA_UI_PAPER);
        akira_display_text(x + 5, y + 3, text, AKIRA_UI_PAPER);
    }
}

/* ---- alert ------------------------------------------------------------ */

void akira_ui_alert(const char *title, const char *subtitle)
{
    int W, H;
    akira_display_get_size(&W, &H);
    akira_display_clear(AKIRA_UI_INK);

    /* Non-actionable message inside an elevated rounded dither-shadow card. */
    int bw = W - 80, bh = 130;
    int bx = (W - bw) / 2, by = (H - bh) / 2;
    akira_ui_dither_card(bx, by, bw, bh, 12, /*selected=*/false, /*shadow=*/5);

    int cx = bx + bw / 2;
    int apex_y = by + 18, ts = 16;
    akira_display_triangle(cx, apex_y, cx - ts, apex_y + ts * 2,
                           cx + ts, apex_y + ts * 2, AKIRA_UI_PAPER);
    akira_display_vline(cx, apex_y + 8, ts - 6, AKIRA_UI_PAPER);
    akira_display_rect(cx - 1, apex_y + ts + 2, 2, 2, AKIRA_UI_PAPER);

    if (title && title[0]) {
        akira_display_text(cx - text_w(title) / 2, by + bh / 2 + 6, title, AKIRA_UI_PAPER);
    }
    if (subtitle && subtitle[0]) {
        akira_display_text(cx - text_w(subtitle) / 2, by + bh / 2 + 22, subtitle,
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

    /* The reserved Capability-Guard marker: the system's largest dither-shadow
     * offset (6px) + a rounded card + a heavy 3px outline. Weight = stakes. */
    dither_rounded(bx + 6, by + 6, bw, bh, 14, AKIRA_UI_PAPER);
    akira_display_rounded_rect_fill(bx, by, bw, bh, 14, AKIRA_UI_INK);
    for (int i = 0; i < 3; i++) {
        akira_display_rounded_rect(bx + i, by + i, bw - 2 * i, bh - 2 * i, 14 - i,
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

    /* Both buttons use the same selected/unselected language: filled +
     * inverted text when selected, outline-only when not — a hardcoded
     * fill or a bare outline alone doesn't reliably show which is chosen. */
    int btn_y = by + bh - 44;
    const char *cancel = "cancel";
    int clw = text_w(cancel) + 20;
    int cxl = bx + bw / 4 - clw / 2;
    akira_ui_dither_card(cxl, btn_y - 4, clw, 28, 8, /*selected=*/sel == 0,
                         /*shadow=*/sel == 0 ? 3 : 0);
    akira_display_text(cxl + (clw - text_w(cancel)) / 2, btn_y + 5, cancel,
                       sel == 0 ? AKIRA_UI_INK : AKIRA_UI_PAPER);

    int cbx = bx + bw / 2 + 8, cbw = bw / 2 - 24, cbh = 28;
    akira_ui_dither_card(cbx, btn_y, cbw, cbh, 8, /*selected=*/sel == 1,
                         /*shadow=*/sel == 1 ? 3 : 0);
    akira_display_text(cbx + (cbw - text_w("confirm")) / 2, btn_y + 5, "confirm",
                       sel == 1 ? AKIRA_UI_INK : AKIRA_UI_PAPER);

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
