/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file akira_ui.h
 * @brief AkiraConsole shared UI kit — native (akira_display_*) implementation.
 *
 * One consistent visual language for every shell screen, built from four
 * primitives only (see akira_ui.c):
 *   1. outline      — 1px stroke ...... neutral / idle / unselected
 *   2. solid fill   — inverted paper .. selected / active / "on"
 *   3. dither       — 50% checker ..... caution / disabled / non-actionable
 *   4. heavy stroke — 3px ............. Capability-Guard confirmation ONLY
 *
 * True 1-bit: the only two colours permitted are AKIRA_UI_INK and
 * AKIRA_UI_PAPER. No grayscale, no hues — every state is a pixel-weight signal.
 *
 * One deliberate exception: the battery widget (akira_ui_battery) uses
 * semantic accents — green while charging / full, amber when low, red when
 * critical — because power state is the one thing worth a colour glance.
 *
 * This is the native/shell half. A WASM-app mirror with identical signatures
 * (wrapping the akira_api.h subset) lives under AkiraSDK/wasm_apps/common/.
 */

#ifndef AKIRA_UI_H
#define AKIRA_UI_H

#include <stdint.h>
#include <stdbool.h>

/* The only two colours the kit permits (RGB565).
 *
 * THEME: light (black on white).  Swap these two values to invert the whole
 * kit back to dark (white on black) — nothing else needs to change.
 *
 * Note the historical names are the opposite way round from typography:
 * AKIRA_UI_INK is the screen BACKGROUND and AKIRA_UI_PAPER is the FOREGROUND.
 * They are kept because AkiraSDK/wasm_apps/common/akira_ui.h exposes them to
 * WASM apps as public API. */
#define AKIRA_UI_INK   0xFFFFu /* white — screen background / idle stroke   */
#define AKIRA_UI_PAPER 0x0000u /* black — foreground / selected fill        */

/* 8x13 px character cell → 50 cols x 18 rows on 400x240 (kit grid). The
 * shell font advances 8 px/char horizontally, so text width == strlen*8. */
#define AKIRA_UI_CELL_W 8
#define AKIRA_UI_CELL_H 13

/* Kit nominal status-bar height is 16 px; the shell currently reserves y=0..24
 * for chrome, so we keep 24 here to migrate without re-tuning every screen's
 * content origin. Tighten to 16 once the carousel geometry is retuned. */
#define AKIRA_UI_STATUSBAR_H 24

/* Fill mode for 1-bit icon blits. */
typedef enum {
    AKIRA_UI_SOLID,       /* set bits → fg, clear bits → bg                 */
    AKIRA_UI_TRANSPARENT, /* set bits → fg, clear bits untouched            */
    AKIRA_UI_DITHER,      /* set bits → fg on 50% checker (caution/disabled)*/
} akira_ui_fill_t;

/* Battery state for the widget.  Values match akira_batt_state_t in
 * power_manager.h so the shell can cast straight across. */
typedef enum {
    AKIRA_UI_BATT_UNKNOWN = 0,
    AKIRA_UI_BATT_DISCHARGING,
    AKIRA_UI_BATT_CHARGING,
    AKIRA_UI_BATT_FULL,
    AKIRA_UI_BATT_NOT_CHARGING, /* on external power, charger idle/fault */
} akira_ui_batt_t;

/* Status-bar model. */
typedef struct {
    const char     *title;       /* left label; NULL/"" → none              */
    const char     *clock;       /* right-aligned text (e.g. "12:00")       */
    int             battery_pct; /* 0..100; <0 to hide the gauge            */
    akira_ui_batt_t battery_state;
    bool            show_wifi;
    bool            wifi_on;      /* off → drawn dithered (present but idle) */
    bool            show_bt;
    bool            bt_on;
} akira_ui_status_t;

/* Font ids for akira_ui_text_w(). */
#define AKIRA_UI_FONT_SMALL 0 /* akira_display_text       — 8 px advance  */
#define AKIRA_UI_FONT_LARGE 1 /* akira_display_text_large — 12 px advance */
#define AKIRA_UI_FONT_HUGE  2 /* akira_display_text_huge  — 17 px advance */

/* ---- Primitives ------------------------------------------------------- */

/** Blit a 1-bit glyph (uint32 rows, bit31 leftmost) at integer scale. */
void akira_ui_icon_1bpp(int x, int y, int scale, const uint32_t *icon,
                        int w, int rows, uint16_t fg, uint16_t bg,
                        akira_ui_fill_t fill);

/** Pixel width of @p s in one of the AKIRA_UI_FONT_* faces. */
int akira_ui_text_w(const char *s, int font);

/** The top bar — title left; Wi-Fi, BT, battery and clock right. */
void akira_ui_status_bar(const akira_ui_status_t *s);

/**
 * Battery icon at integer @p scale (scale 1 = 25x10 incl. terminal nub).
 * Five segments, coloured by state; while charging the segments sweep from
 * the current level to full and a lightning bolt is drawn (inside the body
 * from scale 2 up).  @p now_ms drives the animation — pass k_uptime_get().
 * @return frame index of the animation (changes when a repaint is due).
 */
int akira_ui_battery(int x, int y, int scale, int pct, akira_ui_batt_t state,
                     int64_t now_ms);

/** Width / height of akira_ui_battery() at @p scale. */
#define AKIRA_UI_BATT_W(scale) (25 * (scale))
#define AKIRA_UI_BATT_H(scale) (10 * (scale))

/** Animation frame akira_ui_battery() would draw now (to detect repaints). */
int akira_ui_battery_frame(int pct, akira_ui_batt_t state, int64_t now_ms);

/** Lightning bolt, 7x11 px at scale 1. */
void akira_ui_bolt(int x, int y, int scale, uint16_t color);

/**
 * Footer hint bar across the bottom 24 px.  Each side is a '|'-separated
 * list of "KEY:Label" hints, e.g. "A:Open|HOME:Options"; the key is drawn
 * as an inverted pill.  NULL or "" leaves that side empty.
 */
void akira_ui_footer(const char *left, const char *right);

/** Small animated busy spinner (8 dots on a ring), radius @p r. */
void akira_ui_spinner(int cx, int cy, int r, int64_t now_ms);

/** List row with optional right-aligned meta and a 0..5 block meter.
 *  Selected row inverts (PAPER fill / INK text) — never underline/tint. */
void akira_ui_list_row(int y, int h, const char *text, const char *meta,
                       int meter_0_5, bool selected);

/**
 * Dither-shadow rounded card — the signature Playdate-style elevation
 * primitive that every tappable element composes from. Draws a rounded card
 * plus a checkerboard-dithered copy of its silhouette offset down-right by
 * @p shadow_offset px (3-6; larger = more elevated). Selected = paper-on-ink
 * fill; idle = ink body + paper outline. The dither shadow stays visible
 * behind the selected fill. 1-bit only — depth is the dither, never grey.
 */
void akira_ui_dither_card(int x, int y, int w, int h, int radius,
                          bool selected, int shadow_offset);

/** Home-menu-style icon tile — a dither-shadow card with icon + label. */
void akira_ui_grid_tile(int x, int y, int w, int h, int scale,
                        const uint32_t *icon, int icon_w, int icon_rows,
                        const char *label, bool selected);

/** Button — idle outline / pressed solid fill. */
void akira_ui_button(int x, int y, int w, int h, const char *label,
                     bool pressed);

/** Toggle — off outline knob-left / on solid fill knob-right. */
void akira_ui_toggle(int x, int y, const char *label, bool on);

/** Outline track + proportional fill (TOTP / tuning bars). */
void akira_ui_meter(int x, int y, int w, int h, int value, int max);

/** Inline classifier badge; dithered flags a caution/non-replayable tag. */
void akira_ui_tag(int x, int y, const char *text, bool dithered);

/**
 * Capability-Guard confirmation dialog — the ONLY 3px-stroke element in the
 * kit.  Pushed as a modal screen over whatever is showing; LEFT/RIGHT pick,
 * A answers, B cancels.  Route every restricted-syscall gate (RF_RAW_TX,
 * WIFI_DEAUTH, CRED_EXPORT, OTA_TRIGGER, APP_INSTALL, …) through this.
 *
 * Non-blocking: @p on_result runs on the shell thread once the user answers,
 * after the dialog has been popped (so it may push another screen).
 *
 * @param capability_name  shown literally, e.g. "RF_RAW_TX"
 * @param question         plain-language prompt (copied)
 * @param should_cancel    optional; polled every frame — returning true
 *                         answers "cancel" (e.g. USB cable pulled mid-prompt)
 * @param on_result        receives true for confirm, false for cancel
 */
void akira_ui_confirm_open(const char *capability_name, const char *question,
                           bool (*should_cancel)(void),
                           void (*on_result)(bool confirmed, void *ctx),
                           void *ctx);

#endif /* AKIRA_UI_H */
