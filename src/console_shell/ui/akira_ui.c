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
 * The 3px stroke is reserved for akira_ui_confirm_open() so a user knows a
 * guarded syscall is being gated without reading the text.
 *
 * Colour appears in exactly one place: the battery widget's semantic accents.
 */

#include "akira_ui.h"
#include "screen.h"
#include "../shell_theme.h"

#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <api/akira_display_api.h>
#include <api/akira_input_api.h>

/* ---- helpers ---------------------------------------------------------- */

static inline int text_w(const char *s) { return (int)strlen(s) * AKIRA_UI_CELL_W; }

int akira_ui_text_w(const char *s, int font)
{
    static const uint8_t advance[] = { 8, 12, 17 };

    if (!s || font < 0 || font >= (int)ARRAY_SIZE(advance)) {
        return 0;
    }
    return (int)strlen(s) * advance[font];
}

/* Status glyphs, 1-bit, bit31 = leftmost. */
#define GLYPH_ROWS 16

static const uint32_t ICON_WIFI[GLYPH_ROWS] = {
    0b00001111111111110000000000000000u,
    0b00111000000000011100000000000000u,
    0b01100000000000000110000000000000u,
    0b10000111111111100001000000000000u,
    0b00001100000001100000000000000000u,
    0b00000011111110000000000000000000u,
    0b00000000110000000000000000000000u,
    0b00000001111000000000000000000000u,
    0b00000001111000000000000000000000u,
};
#define ICON_WIFI_W 20

static const uint32_t ICON_BT[GLYPH_ROWS] = {
    0b00110000000000000000000000000000u,
    0b00101000000000000000000000000000u,
    0b10100100000000000000000000000000u,
    0b01101000000000000000000000000000u,
    0b00110000000000000000000000000000u,
    0b01101000000000000000000000000000u,
    0b10100100000000000000000000000000u,
    0b00101000000000000000000000000000u,
    0b00110000000000000000000000000000u,
};
#define ICON_BT_W 7

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

/* ---- battery widget ---------------------------------------------------- */

#define BATT_SEGS          5
#define BATT_SWEEP_MS      320  /* one charging-sweep step */
#define BATT_PULSE_MS      500  /* critical outline blink half-period */
#ifdef CONFIG_AKIRA_BATTERY_LOW_THRESHOLD
#define BATT_LOW_PCT       CONFIG_AKIRA_BATTERY_LOW_THRESHOLD
#define BATT_CRIT_PCT      CONFIG_AKIRA_BATTERY_CRITICAL_THRESHOLD
#else
#define BATT_LOW_PCT       15
#define BATT_CRIT_PCT      5
#endif

static int batt_lit(int pct)
{
    pct = CLAMP(pct, 0, 100);
    return (pct + (100 / BATT_SEGS) - 1) / (100 / BATT_SEGS); /* ceil */
}

int akira_ui_battery_frame(int pct, akira_ui_batt_t state, int64_t now_ms)
{
    int lit = batt_lit(pct);

    if (state == AKIRA_UI_BATT_CHARGING && lit < BATT_SEGS) {
        return 1 + (int)((now_ms / BATT_SWEEP_MS) % (BATT_SEGS - lit + 1));
    }
    if (state != AKIRA_UI_BATT_CHARGING && state != AKIRA_UI_BATT_FULL &&
        state != AKIRA_UI_BATT_NOT_CHARGING && pct <= BATT_CRIT_PCT) {
        return 1 + (int)((now_ms / BATT_PULSE_MS) & 1);
    }
    return 0;
}

void akira_ui_bolt(int x, int y, int scale, uint16_t color)
{
    int s = scale;

    akira_display_triangle_fill(x + 5 * s, y, x, y + 6 * s, x + 4 * s, y + 6 * s, color);
    akira_display_triangle_fill(x + 3 * s, y + 4 * s, x + 7 * s, y + 4 * s,
                                x + 2 * s, y + 11 * s, color);
}

int akira_ui_battery(int x, int y, int scale, int pct, akira_ui_batt_t state,
                     int64_t now_ms)
{
    const int s = MAX(scale, 1);
    const int bw = 23 * s, bh = AKIRA_UI_BATT_H(s);
    const bool external = state == AKIRA_UI_BATT_CHARGING ||
                          state == AKIRA_UI_BATT_FULL ||
                          state == AKIRA_UI_BATT_NOT_CHARGING;
    int frame = akira_ui_battery_frame(pct, state, now_ms);
    int lit = batt_lit(pct);
    uint16_t seg_c, frame_c = AKIRA_UI_PAPER;

    if (state == AKIRA_UI_BATT_CHARGING || state == AKIRA_UI_BATT_FULL) {
        seg_c = SHELL_C_CHARGE;
    } else if (!external && pct <= BATT_CRIT_PCT) {
        seg_c = SHELL_C_CRIT;
        frame_c = (frame == 2) ? SHELL_C_CRIT : AKIRA_UI_PAPER;
    } else if (!external && pct <= BATT_LOW_PCT) {
        seg_c = SHELL_C_WARN;
    } else {
        seg_c = AKIRA_UI_PAPER;
    }

    if (state == AKIRA_UI_BATT_FULL) {
        lit = BATT_SEGS;
    } else if (state == AKIRA_UI_BATT_CHARGING && frame > 0) {
        lit += frame - 1; /* sweep: one more segment per step, then restart */
    }

    /* Body: s-px outline, rounded, with the terminal nub on the right. */
    akira_display_rounded_rect_fill(x, y, bw, bh, 2 * s, AKIRA_UI_INK);
    for (int i = 0; i < s; i++) {
        akira_display_rounded_rect(x + i, y + i, bw - 2 * i, bh - 2 * i,
                                   MAX(2 * s - i, 1), frame_c);
    }
    akira_display_rect(x + bw, y + 3 * s, 2 * s, bh - 6 * s, frame_c);

    /* Five 3s-wide segments with 1s gaps fill the 19s-wide interior. */
    for (int i = 0; i < BATT_SEGS; i++) {
        akira_display_rect(x + 2 * s + i * 4 * s, y + 2 * s, 3 * s, bh - 4 * s,
                           i < lit ? seg_c : AKIRA_UI_INK);
    }

    /* Big icons carry the bolt inside the body: ink with a paper outline so
     * it reads over lit and unlit segments alike. */
    if (s >= 2 && (state == AKIRA_UI_BATT_CHARGING ||
                   state == AKIRA_UI_BATT_NOT_CHARGING)) {
        int bs = MAX(s * 2 / 3, 1);
        int gx = x + (bw - 7 * bs) / 2, gy = y + (bh - 11 * bs) / 2;

        akira_ui_bolt(gx - 1, gy, bs, AKIRA_UI_PAPER);
        akira_ui_bolt(gx + 1, gy, bs, AKIRA_UI_PAPER);
        akira_ui_bolt(gx, gy - 1, bs, AKIRA_UI_PAPER);
        akira_ui_bolt(gx, gy + 1, bs, AKIRA_UI_PAPER);
        akira_ui_bolt(gx, gy, bs,
                      state == AKIRA_UI_BATT_CHARGING ? AKIRA_UI_INK : SHELL_C_GRAY);
    }
    return frame;
}

/* ---- spinner ------------------------------------------------------------ */

void akira_ui_spinner(int cx, int cy, int r, int64_t now_ms)
{
    /* 8 dots on a ring; the head is solid, the tail fades through the grays. */
    static const int8_t ring[8][2] = {
        { 0, -10 }, { 7, -7 }, { 10, 0 }, { 7, 7 },
        { 0, 10 }, { -7, 7 }, { -10, 0 }, { -7, -7 },
    };
    int head = (int)((now_ms / 90) % 8);

    for (int i = 0; i < 8; i++) {
        int age = (head - i + 8) % 8;
        uint16_t c = age == 0 ? AKIRA_UI_PAPER
                   : age < 3 ? SHELL_C_GRAY
                   : SHELL_C_DKGRAY;
        int dr = age == 0 ? MAX(r / 4, 2) : MAX(r / 6, 1);

        akira_display_circle_fill(cx + ring[i][0] * r / 10, cy + ring[i][1] * r / 10,
                                  dr, c);
    }
}

/* ---- status bar ------------------------------------------------------- */

void akira_ui_status_bar(const akira_ui_status_t *s)
{
    /* Shell geometry, not the panel: the shell lays out SCR_W x SCR_H. */
    const int W = SCR_W;
    const int bar_h = AKIRA_UI_STATUSBAR_H;
    const int txt_y = (bar_h - 10) / 2;
    const int64_t now = k_uptime_get();

    /* Clear the band (content may have scrolled under it), then the pill. */
    akira_display_rect(0, 0, W, bar_h + 2, AKIRA_UI_INK);
    int pill_h = bar_h - 2, pr = pill_h / 2;
    akira_display_rounded_rect_fill(2, 1, W - 4, pill_h, pr, AKIRA_UI_INK);
    akira_display_rounded_rect(2, 1, W - 4, pill_h, pr, AKIRA_UI_PAPER);

    /* --- right cluster, laid out right-to-left --- */
    int x = W - 12;

    if (s->clock && s->clock[0]) {
        x -= text_w(s->clock);
        akira_display_text(x, txt_y, s->clock, AKIRA_UI_PAPER);
        x -= 10;
    }

    if (s->battery_pct >= 0) {
        char buf[8];
        int pct = MIN(s->battery_pct, 100);

        snprintf(buf, sizeof(buf), "%d%%", pct);
        x -= text_w(buf);
        akira_display_text(x, txt_y, buf, AKIRA_UI_PAPER);
        x -= 4 + AKIRA_UI_BATT_W(1);
        akira_ui_battery(x, (bar_h - AKIRA_UI_BATT_H(1)) / 2, 1, pct,
                         s->battery_state, now);
        if (s->battery_state == AKIRA_UI_BATT_CHARGING ||
            s->battery_state == AKIRA_UI_BATT_NOT_CHARGING) {
            x -= 9;
            akira_ui_bolt(x, (bar_h - 11) / 2, 1,
                          s->battery_state == AKIRA_UI_BATT_CHARGING
                              ? SHELL_C_CHARGE : SHELL_C_GRAY);
        }
        x -= 8;
    }

    int icon_y = (bar_h - 9) / 2;
    if (s->show_bt) {
        x -= ICON_BT_W;
        akira_ui_icon_1bpp(x, icon_y, 1, ICON_BT, ICON_BT_W, 9,
                           AKIRA_UI_PAPER, AKIRA_UI_INK,
                           s->bt_on ? AKIRA_UI_TRANSPARENT : AKIRA_UI_DITHER);
        x -= 8;
    }
    if (s->show_wifi) {
        x -= ICON_WIFI_W;
        akira_ui_icon_1bpp(x, icon_y, 1, ICON_WIFI, ICON_WIFI_W, 9,
                           AKIRA_UI_PAPER, AKIRA_UI_INK,
                           s->wifi_on ? AKIRA_UI_TRANSPARENT : AKIRA_UI_DITHER);
        x -= 8;
    }

    /* --- title on the left, clipped to what the cluster left free --- */
    if (s->title && s->title[0]) {
        char t[40];
        int max_chars = MAX((x - 12) / AKIRA_UI_CELL_W, 1);

        strncpy(t, s->title, sizeof(t) - 1);
        t[sizeof(t) - 1] = '\0';
        if ((int)strlen(t) > max_chars) {
            t[max_chars] = '\0';
            if (max_chars > 2) {
                t[max_chars - 1] = '.';
                t[max_chars - 2] = '.';
            }
        }
        akira_display_text(12, txt_y, t, AKIRA_UI_PAPER);
    }
}

/* ---- footer hint bar -------------------------------------------------- */

/* Draws one "KEY:Label" hint at x, returns its width. */
static int footer_hint(int x, int y, const char *hint, int len, bool measure)
{
    const char *colon = memchr(hint, ':', len);
    char key[8] = "", label[24] = "";

    if (colon) {
        snprintf(key, sizeof(key), "%.*s", (int)(colon - hint), hint);
        snprintf(label, sizeof(label), "%.*s", (int)(len - (colon - hint) - 1), colon + 1);
    } else {
        snprintf(label, sizeof(label), "%.*s", len, hint);
    }

    int kw = key[0] ? text_w(key) + 8 : 0;
    int w = kw + (key[0] && label[0] ? 4 : 0) + text_w(label);

    if (!measure) {
        if (key[0]) {
            akira_display_rounded_rect_fill(x, y + 5, kw, 14, 7, AKIRA_UI_PAPER);
            akira_display_text(x + 4, y + 7, key, AKIRA_UI_INK);
        }
        if (label[0]) {
            akira_display_text(x + w - text_w(label), y + 7, label, AKIRA_UI_PAPER);
        }
    }
    return w;
}

/* Walks a '|'-separated hint list; returns total width. */
static int footer_side(int x, int y, const char *hints, bool measure)
{
    int total = 0;

    while (hints && *hints) {
        const char *bar = strchr(hints, '|');
        int len = bar ? (int)(bar - hints) : (int)strlen(hints);

        if (total) {
            total += 12;
        }
        total += footer_hint(x + total, y, hints, len, measure);
        hints = bar ? bar + 1 : NULL;
    }
    return total;
}

void akira_ui_footer(const char *left, const char *right)
{
    /* Shell geometry, not the panel: the shell lays out SCR_W x SCR_H. */
    const int W = SCR_W, H = SCR_H;
    const int y = H - 24;

    akira_display_rect(0, y, W, 24, AKIRA_UI_INK);
    for (int x = 0; x < W; x += 2) {
        akira_display_pixel(x, y, AKIRA_UI_PAPER); /* dotted hairline */
    }
    footer_side(10, y, left, false);
    footer_side(W - 10 - footer_side(0, y, right, true), y, right, false);
}

/* ---- list row --------------------------------------------------------- */

void akira_ui_list_row(int y, int h, const char *text, const char *meta,
                       int meter_0_5, bool selected)
{
    /* Shell geometry, not the panel: the shell lays out SCR_W x SCR_H. */
    const int W = SCR_W;
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

/* ---- confirm dialog (the only 3px element) ---------------------------- */

static struct {
    char cap[32];
    char question[64];
    int sel;
    bool (*should_cancel)(void);
    void (*on_result)(bool confirmed, void *ctx);
    void *ctx;
} g_confirm;

static void confirm_finish(bool confirmed)
{
    void (*cb)(bool, void *) = g_confirm.on_result;
    void *ctx = g_confirm.ctx;

    g_confirm.on_result = NULL;
    screen_pop(); /* first, so the callback may push its own screen */
    if (cb) {
        cb(confirmed, ctx);
    }
}

static void confirm_draw(void)
{
    /* Shell geometry, not the panel: the shell lays out SCR_W x SCR_H. */
    const int W = SCR_W, H = SCR_H;
    int bw = W - 64, bh = 156;
    int bx = (W - bw) / 2, by = (H - bh) / 2 + 6;
    int cx = bx + bw / 2;

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
    akira_display_vline(cx, gy + 4, gs - 6, AKIRA_UI_PAPER);
    akira_display_rect(cx, gy + gs - 2, 1, 1, AKIRA_UI_PAPER);

    akira_display_text(cx - text_w(g_confirm.question) / 2, by + 42,
                       g_confirm.question, AKIRA_UI_PAPER);

    char cap[64];
    snprintf(cap, sizeof(cap), "capability: %s", g_confirm.cap);
    akira_display_text(cx - text_w(cap) / 2, by + 60, cap, SHELL_C_GRAY);
    akira_display_text(cx - text_w("restricted action") / 2, by + 74,
                       "restricted action", SHELL_C_GRAY);

    /* Two equal buttons; the selected one is filled + elevated. */
    const int btn_w = (bw - 48) / 2, btn_h = 28, btn_y = by + bh - 44;
    static const char *const labels[2] = { "Cancel", "Confirm" };

    for (int i = 0; i < 2; i++) {
        int bxi = bx + 16 + i * (btn_w + 16);
        bool sel = (g_confirm.sel == i);

        akira_ui_dither_card(bxi, btn_y, btn_w, btn_h, 8, sel, sel ? 3 : 1);
        akira_display_text(bxi + (btn_w - text_w(labels[i])) / 2,
                           btn_y + (btn_h - 10) / 2, labels[i],
                           sel ? AKIRA_UI_INK : AKIRA_UI_PAPER);
    }
}

static void confirm_key(uint32_t just)
{
    if (just & (BIT(AKIRA_BTN_LEFT) | BIT(AKIRA_BTN_RIGHT) |
                BIT(AKIRA_BTN_UP) | BIT(AKIRA_BTN_DOWN))) {
        g_confirm.sel ^= 1;
        screen_invalidate();
    }
    if (just & BIT(AKIRA_BTN_A)) {
        confirm_finish(g_confirm.sel == 1);
    } else if (just & BIT(AKIRA_BTN_B)) {
        confirm_finish(false);
    }
}

static bool confirm_tick(void)
{
    if (g_confirm.should_cancel && g_confirm.should_cancel()) {
        confirm_finish(false);
    }
    return false;
}

static void confirm_leave(void)
{
    /* Popped from outside (e.g. HOME long-press): answer "cancel". */
    if (g_confirm.on_result) {
        void (*cb)(bool, void *) = g_confirm.on_result;

        g_confirm.on_result = NULL;
        cb(false, g_confirm.ctx);
    }
}

static const shell_screen_t g_confirm_screen = {
    .name = "confirm",
    .flags = SCR_F_MODAL,
    .leave = confirm_leave,
    .on_key = confirm_key,
    .tick = confirm_tick,
    .draw = confirm_draw,
};

void akira_ui_confirm_open(const char *capability_name, const char *question,
                           bool (*should_cancel)(void),
                           void (*on_result)(bool confirmed, void *ctx),
                           void *ctx)
{
    if (screen_is_open(&g_confirm_screen)) {
        /* One guarded question at a time: refuse the newcomer. */
        if (on_result) {
            on_result(false, ctx);
        }
        return;
    }
    snprintf(g_confirm.cap, sizeof(g_confirm.cap), "%s",
             capability_name ? capability_name : "?");
    snprintf(g_confirm.question, sizeof(g_confirm.question), "%s",
             question ? question : "");
    g_confirm.sel = 0; /* default to cancel — safest for a guarded action */
    g_confirm.should_cancel = should_cancel;
    g_confirm.on_result = on_result;
    g_confirm.ctx = ctx;
    screen_push(&g_confirm_screen, NULL);
}
