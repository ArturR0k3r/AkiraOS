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

/* Status-bar model. Icons are the shell's native 1-bit format:
 * uint32_t[rows], bit31 = leftmost pixel, up to 32 px wide. */
typedef struct {
    const char     *title;       /* left label; NULL → show battery gauge   */
    const char     *clock;       /* right-aligned text (e.g. "12:00:00")    */
    int             battery_pct; /* 0..100; <0 to hide the gauge            */
    bool            show_wifi;
    bool            wifi_on;      /* off → drawn dithered (present but idle) */
    bool            show_bt;
    bool            bt_on;
    const uint32_t *icon_wifi;   /* 20x16 glyph, or NULL                    */
    const uint32_t *icon_bt;     /* 20x16 glyph, or NULL                    */
} akira_ui_status_t;

/* ---- Primitives ------------------------------------------------------- */

/** Blit a 1-bit glyph (uint32 rows, bit31 leftmost) at integer scale. */
void akira_ui_icon_1bpp(int x, int y, int scale, const uint32_t *icon,
                        int w, int rows, uint16_t fg, uint16_t bg,
                        akira_ui_fill_t fill);

/** The one inverted top bar — app title left, status cluster right. */
void akira_ui_status_bar(const akira_ui_status_t *s);

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

/** Full-screen non-actionable system message (e.g. rolling-code alert). */
void akira_ui_alert(const char *title, const char *subtitle);

/**
 * Capability-Guard confirmation dialog — the ONLY 3px-stroke element in the
 * kit. Blocking: draws over the current frame, drives the button input loop,
 * and returns true (confirm) / false (cancel). Route every restricted-syscall
 * gate (RF_RAW_TX, WIFI_DEAUTH, CRED_EXPORT, OTA_TRIGGER, …) through this.
 *
 * @param capability_name  shown literally, e.g. "RF_RAW_TX"
 * @param question         plain-language prompt, e.g. "confirm TX at 433.92 MHz?"
 */
bool akira_ui_confirm_dialog(const char *capability_name, const char *question);

#endif /* AKIRA_UI_H */
