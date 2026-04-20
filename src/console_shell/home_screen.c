/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_home_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_home_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file home_screen.c
 * @brief AkiraOS "Liquid Crystal" Home Screen.
 *
 * PS5-inspired horizontal carousel + slide-up options panel.
 * Pure akira_display_* renderer — no LVGL.
 *
 * Layout (320x240 landscape):
 *   y=  0..23   Status bar: segmented battery (left) | WiFi/BT icons (centre) | HH:MM:SS (right)
 *   y= 24       1 px white hairline separator
 *   y= 25..214  Carousel zone (190 px, idle)
 *   y=215       1 px white hairline separator
 *   y=216..239  Footer: "A-Open | B-Back | HOME-Options"
 *
 * Options panel (DOWN or HOME):
 *   Carousel compresses to y=25..94  (70 px, ~40%)
 *   Panel slides up from off-screen to y=95..239
 *
 * Animation: cubic ease-in-out, 10 frames x 20 ms = 200 ms.
 * Glass backdrop: checkerboard dither (0x0000 / 0x2104).
 */

#include "home_screen.h"
#include "settings_screen.h"
#include "shell_theme.h"

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <runtime/app_manager/app_manager.h>
#include <lib/akira_time.h>

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#endif

#ifdef CONFIG_BT
#include <connectivity/bluetooth/bt_manager.h>
#endif

/* ---- Screen geometry (home-screen specific) --------------------- */
#define SBAR_Y        0

#define CAR_Y         25
#define CAR_H_IDLE    190
#define CAR_H_OPT     100  /* carousel height when options open */

#define FOOT_Y        216
#define FOOT_H        24

/* options panel always starts at CAR_Y + CAR_H_OPT */
#define OPT_PANEL_Y   (CAR_Y + CAR_H_OPT)  /* 83 */
#define OPT_HDR_H     4   /* hairline gap only, no title */
#define OPT_ITEM_H    40

/* options panel button geometry (leave right strip for scrollbar) */
#define OPT_BTN_X     8
#define OPT_BTN_W     (SCR_W - 30)  /* 290 px — leaves 22 px for scrollbar */
#define OPT_TXT_PAD   10            /* inner horizontal text padding */
#define OPT_SBAR_X    (SCR_W - 14) /* scrollbar track x */
#define OPT_SBAR_W    7            /* scrollbar track width */

/* ---- Animation --------------------------------------------------- */
#define ANIM_FRAMES   10

typedef struct { int start, end, frame; } anim_t;

static int ease_inout(int t)
{
    int t2 = t * t / 256;
    int t3 = t2 * t / 256;
    int v  = 3 * t2 - 2 * t3;
    return (v > 255) ? 255 : (v < 0 ? 0 : v);
}

static int anim_value(const anim_t *a)
{
    if (a->frame >= ANIM_FRAMES) return a->end;
    if (a->frame <= 0)           return a->start;
    int t = a->frame * 255 / (ANIM_FRAMES - 1);
    return a->start + (a->end - a->start) * ease_inout(t) / 255;
}

static bool anim_running(const anim_t *a) { return a->frame < ANIM_FRAMES; }
static void anim_start(anim_t *a, int s, int e) { a->start = s; a->end = e; a->frame = 0; }
static void anim_step(anim_t *a) { if (a->frame < ANIM_FRAMES) a->frame++; }

/* ---- Icons (20x16 px, 1-bit, bit31=leftmost) -------------------- */
#define ICON_W 20
#define ICON_H 16

static const uint32_t ICON_TERMINAL[ICON_H] = {
    0b11111111111111111100000000000000u,
    0b10000000000000000100000000000000u,
    0b10111111111111110100000000000000u,
    0b10100000000000010100000000000000u,
    0b10101100000000010100000000000000u,
    0b10100110000000010100000000000000u,
    0b10100011000000010100000000000000u,
    0b10100110000000010100000000000000u,
    0b10101100000000010100000000000000u,
    0b10100000000000010100000000000000u,
    0b10111111111111110100000000000000u,
    0b10000000000000000100000000000000u,
    0b11111111111111111100000000000000u,
    0b00000110000011000000000000000000u,
    0b00000011000110000000000000000000u,
    0b00001111111110000000000000000000u,
};
static const uint32_t ICON_RADIO[ICON_H] = {
    0b00000000000000001100000000000000u,
    0b00000000000000011000000000000000u,
    0b00000000000000110000000000000000u,
    0b11111111111111111100000000000000u,
    0b10000000000000000100000000000000u,
    0b10011111100011110100000000000000u,
    0b10010000100010000100000000000000u,
    0b10010000100011110100000000000000u,
    0b10010000100010000100000000000000u,
    0b10011111100010000100000000000000u,
    0b10000000000000000100000000000000u,
    0b11111111111111111100000000000000u,
    0b00000110000001100000000000000000u,
    0b00000110000001100000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
};
static const uint32_t ICON_CAMERA[ICON_H] = {
    0b00000111111000000000000000000000u,
    0b11111111111111111100000000000000u,
    0b10000000000000000100000000000000u,
    0b10000011111100000100000000000000u,
    0b10000100000010000100000000000000u,
    0b10001000000001000100000000000000u,
    0b10001011110001000100000000000000u,
    0b10001010010001000100000000000000u,
    0b10001011110001000100000000000000u,
    0b10001000000001000100000000000000u,
    0b10000100000010000100000000000000u,
    0b10000011111100000100000000000000u,
    0b10000000000000000100000000000000u,
    0b11111111111111111100000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
};
static const uint32_t ICON_GAMES[ICON_H] = {
    0b00001111111111110000000000000000u,
    0b00110000000000001100000000000000u,
    0b01000001001100010010000000000000u,
    0b10000001001100100001000000000000u,
    0b10000011111100100001000000000000u,
    0b10000001001100100001000000000000u,
    0b10000001001100010010000000000000u,
    0b01100000000000000001000000000000u,
    0b00011000000000000110000000000000u,
    0b00000110000001111000000000000000u,
    0b00000001111110000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
};
static const uint32_t ICON_SETTINGS[ICON_H] = {
    0b00001110000000000000000000000000u,
    0b00001110000000000000000000000000u,
    0b01111111111000000000000000000000u,
    0b11000000001100000000000000000000u,
    0b10000111000100000000000000000000u,
    0b01111111111000000000000000000000u,
    0b00001110000000000000000000000000u,
    0b00001110000000000000000000000000u,
    0b00011111000000000000000000000000u,
    0b00110001100000000000000000000000u,
    0b01110001110000000000000000000000u,
    0b00110001100000000000000000000000u,
    0b00011111000000000000000000000000u,
    0b00001110000000000000000000000000u,
    0b00001110000000000000000000000000u,
    0b00000000000000000000000000000000u,
};
static const uint32_t ICON_SDCARD[ICON_H] = {
    0b11111100000000000000000000000000u,
    0b11111111111111111100000000000000u,
    0b10000000000000000100000000000000u,
    0b10001110010011100100000000000000u,
    0b10010001011010000100000000000000u,
    0b10001110011010000100000000000000u,
    0b10000001010010000100000000000000u,
    0b10001110010011100100000000000000u,
    0b10000000000000000100000000000000u,
    0b10000000000000000100000000000000u,
    0b10000000000000000100000000000000u,
    0b11111111111111111100000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
};
static const uint32_t ICON_INFO[ICON_H] = {
    0b00001111110000000000000000000000u,
    0b00111000011100000000000000000000u,
    0b01100000000110000000000000000000u,
    0b01000011000010000000000000000000u,
    0b10000011000001000000000000000000u,
    0b10000000000001000000000000000000u,
    0b10000011000001000000000000000000u,
    0b10000011000001000000000000000000u,
    0b10000011000001000000000000000000u,
    0b01000111100010000000000000000000u,
    0b01100000000110000000000000000000u,
    0b00111000011100000000000000000000u,
    0b00001111110000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
};
static const uint32_t ICON_WIFI[ICON_H] = {
    0b00001111111111110000000000000000u,
    0b00111000000000011100000000000000u,
    0b01100000000000000110000000000000u,
    0b10000111111111100001000000000000u,
    0b00001100000001100000000000000000u,
    0b00000011111110000000000000000000u,
    0b00000000110000000000000000000000u,
    0b00000001111000000000000000000000u,
    0b00000001111000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
};
static const uint32_t ICON_BT[ICON_H] = {
    0b00000011000000000000000000000000u,
    0b00000101000000000000000000000000u,
    0b00010011000000000000000000000000u,
    0b00001101000000000000000000000000u,
    0b00000011000000000000000000000000u,
    0b00001101000000000000000000000000u,
    0b00010011000000000000000000000000u,
    0b00000101000000000000000000000000u,
    0b00000011000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
};
static const uint32_t ICON_BATTERY[ICON_H] = {
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b11111111111111110000000000000000u,
    0b10000000000000010110000000000000u,
    0b10111111111111010110000000000000u,
    0b10111111111111010110000000000000u,
    0b10000000000000010000000000000000u,
    0b11111111111111110000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
};

/* ---- Tile model -------------------------------------------------- */
typedef enum { TILE_WASM, TILE_BUILTIN } tile_type_t;

typedef struct {
    tile_type_t    type;
    char           name[APP_NAME_MAX_LEN];
    const uint32_t *icon;
    app_state_t    state;
    void           (*action)(void);
} ui_tile_t;

static void action_settings(void);
static void action_sdcard(void);
static void action_sysinfo(void);

#define BUILTIN_COUNT 3
static const ui_tile_t BUILTINS[BUILTIN_COUNT] = {
    { TILE_BUILTIN, "SETTINGS", ICON_SETTINGS, 0, action_settings },
    { TILE_BUILTIN, "SD CARD",  ICON_SDCARD,   0, action_sdcard   },
    { TILE_BUILTIN, "SYS INFO", ICON_INFO,     0, action_sysinfo  },
};

#define MAX_ALL_TILES  (CONFIG_AKIRA_APP_MAX_INSTALLED + BUILTIN_COUNT)
static ui_tile_t g_all_tiles[MAX_ALL_TILES];
static int       g_total_tiles;
static int       g_last_n_installed;

/* ---- Carousel state --------------------------------------------- */
static int  g_sel;
static bool g_visible;

/* ---- Options panel state ---------------------------------------- */
typedef enum { UI_HOME, UI_OPTIONS } ui_state_t;
static ui_state_t g_ui_state;
static int        g_opts_focused;
static int        g_opts_scroll;
static int        g_opts_count;

#define OPTS_WASM_COUNT    3
#define OPTS_BUILTIN_COUNT 2
static const char *s_opts_wasm[OPTS_WASM_COUNT]      = { "START",  "UNINSTALL", "CANCEL" };
static const char *s_opts_builtin[OPTS_BUILTIN_COUNT] = { "OPEN",   "CANCEL" };

/* ---- Animation state -------------------------------------------- */
static anim_t g_anim_car_h;
static anim_t g_anim_panel_y;
static anim_t g_anim_focus_pop; /* focus tile vertical pop (px below → 0) */
static bool   g_dirty;

#define POP_PX 8   /* max drop distance for the focus pop */
/* Start mid-curve so the pop only runs the ease-out tail (~6 frames, 120 ms) */
static void anim_start_pop(anim_t *a) {
    a->start = POP_PX; a->end = 0; a->frame = ANIM_FRAMES - 6;
}

/* ---- Clock / battery / connectivity ----------------------------- */
static char g_time_str[10] = "00:00:00";
static char g_batt_str[8]  = "100%";
static bool g_wifi_conn;
static bool g_bt_conn;

/* ================================================================== */
/* Draw helpers                                                        */
/* ================================================================== */

static void draw_icon_nx(int x, int y, int scale,
                         const uint32_t *icon, uint16_t fg, uint16_t bg)
{
    for (int r = 0; r < ICON_H; r++) {
        uint32_t bits = icon[r];
        for (int c = 0; c < ICON_W; c++) {
            uint16_t pc = (bits & (1u << (31 - c))) ? fg : bg;
            akira_display_rect(x + c * scale, y + r * scale, scale, scale, pc);
        }
    }
}

static void draw_centred_text(int x, int y, int w, const char *s,
                               uint16_t fg, uint16_t bg)
{
    char clipped[64];
    int max_chars = w / 8;
    if (max_chars < 1) max_chars = 1;
    int slen = (int)strlen(s);
    if (slen > max_chars) {
        int keep = max_chars > 2 ? max_chars - 2 : max_chars;
        strncpy(clipped, s, keep);
        if (max_chars > 2) { clipped[keep] = '.'; clipped[keep+1] = '.'; clipped[keep+2] = '\0'; }
        else clipped[keep] = '\0';
        s = clipped;
    }
    int tw = (int)strlen(s) * 8;
    int lx = x + (tw < w ? (w - tw) / 2 : 0);
    akira_display_rect(x, y, w, 10, bg);
    akira_display_text(lx, y, s, fg);
}

/*
 * glass_rect — Liquid Glass panel (reference-matched).
 *
 * Layers (outer → inner):
 *   1. Frosted gray body fill
 *   2. Outer white rim  — bright lit glass edge
 *   3. Inner dark rim   — glass wall depth/shadow
 *   4. Top highlight    — horizontal sheen across top third
 *   5. Top-left glint   — bright white oval, like the reference image
 */
static void glass_rect(int x, int y, int w, int h, int r)
{
    /* 1. Black fill (transparent/glass look on black bg) */
    akira_display_rounded_rect_fill(x, y, w, h, r, C_BLACK);
    /* 2. Outer bright rim */
    akira_display_rounded_rect(x, y, w, h, r, C_WHITE);
    /* 3. Inner dark rim */
    if (w > 4 && h > 4) {
        int ri = (r > 1) ? r - 1 : 0;
        akira_display_rounded_rect(x + 1, y + 1, w - 2, h - 2, ri, C_DKGRAY);
    }
    /* 4. Top sheen — inset by r to follow corner curve */
    int ti = r + 2;
    if (w > ti * 2 && h > 6) {
        akira_display_hline(x + ti, y + 3, w - ti * 2, C_GLASS_HILIT);
    }
    /* 5. Top-left corner glint */
    if (w > 12 && h > 7) {
        int gw = w / 5;
        if (gw > 14) gw = 14;
        akira_display_rect(x + ti, y + 3, gw, 2, C_WHITE);
        if (gw > 4) akira_display_hline(x + ti, y + 5, gw / 2, C_GLASS_HILIT);
    }
    /* 6. Bottom reflection sheen — inset by r */
    if (w > ti * 2 && h > 8) {
        akira_display_hline(x + ti, y + h - 4, w - ti * 2, C_DKGRAY);
    }
    /* 7. Bottom-right glint */
    if (w > 12 && h > 9) {
        int gw = w / 6;
        if (gw > 10) gw = 10;
        akira_display_hline(x + w - ti - gw, y + h - 4, gw, C_GLASS_HILIT);
    }
}

/*
 * glass_rect_focus — Selected/active Liquid Glass panel.
 * Double white rim (thicker bright edge) + all body layers.
 */
static void glass_rect_focus(int x, int y, int w, int h, int r)
{
    /* 1. Black fill (glass look) */
    akira_display_rounded_rect_fill(x, y, w, h, r, C_BLACK);
    /* 2. Outer rim */
    akira_display_rounded_rect(x, y, w, h, r, C_WHITE);
    /* 3. Second white rim — thick lit glass wall */
    if (w > 2 && h > 2) {
        int r1 = (r > 0) ? r - 1 : 0;
        akira_display_rounded_rect(x + 1, y + 1, w - 2, h - 2, r1, C_WHITE);
    }
    /* 4. Inner dark depth rim */
    if (w > 6 && h > 6) {
        int r2 = (r > 1) ? r - 2 : 0;
        akira_display_rounded_rect(x + 2, y + 2, w - 4, h - 4, r2, C_DKGRAY);
    }
    /* 5. Top sheen — inset by r to follow corner curve */
    int ti = r + 3;
    if (w > ti * 2 && h > 8) {
        akira_display_hline(x + ti, y + 4, w - ti * 2, C_GLASS_HILIT);
    }
    /* 6. Top-left glint */
    if (w > 14 && h > 9) {
        int gw = w / 5;
        if (gw > 16) gw = 16;
        akira_display_rect(x + ti, y + 4, gw, 2, C_WHITE);
        if (gw > 4) akira_display_hline(x + ti, y + 6, gw / 2, C_GLASS_HILIT);
    }
    /* 7. Bottom reflection sheen — inset by r */
    if (w > ti * 2 && h > 10) {
        akira_display_hline(x + ti, y + h - 5, w - ti * 2, C_DKGRAY);
    }
    /* 8. Bottom-right glint */
    if (w > 14 && h > 11) {
        int gw = w / 6;
        if (gw > 12) gw = 12;
        akira_display_hline(x + w - ti - gw, y + h - 5, gw, C_WHITE);
        akira_display_hline(x + w - ti - gw, y + h - 4, gw / 2, C_GLASS_HILIT);
    }
}

/*
 * glass_rect_dim — Dimmed Liquid Glass panel (non-focused / adjacent tiles).
 * Same shape as glass_rect but uses dark rim and a subtle dim glint.
 */
static void glass_rect_dim(int x, int y, int w, int h, int r)
{
    /* 1. Black fill */
    akira_display_rounded_rect_fill(x, y, w, h, r, C_BLACK);
    /* 2. Dark outer rim */
    akira_display_rounded_rect(x, y, w, h, r, C_DKGRAY);
    /* 3. Inner shadow rim */
    if (w > 4 && h > 4) {
        int ri = (r > 1) ? r - 1 : 0;
        akira_display_rounded_rect(x + 1, y + 1, w - 2, h - 2, ri, C_BLACK);
    }
    /* 4. Dim top sheen — inset by r */
    int ti = r + 2;
    if (w > ti * 2 && h > 6) {
        akira_display_hline(x + ti, y + 3, w - ti * 2, C_DKGRAY);
    }
    /* 5. Dim glint strip */
    if (w > 12 && h > 7) {
        int gw = w / 5;
        if (gw > 14) gw = 14;
        akira_display_hline(x + ti, y + 3, gw, C_GRAY);
    }
    /* 6. Bottom dim reflection — inset by r */
    if (w > ti * 2 && h > 8) {
        akira_display_hline(x + ti, y + h - 4, w - ti * 2, C_BLACK);
    }
    /* 7. Bottom-right dim glint */
    if (w > 12 && h > 9) {
        int gw = w / 6;
        if (gw > 10) gw = 10;
        akira_display_hline(x + w - ti - gw, y + h - 4, gw, C_DKGRAY);
    }
}

/* ================================================================== */
/* Status bar                                                          */
/* ================================================================== */

static void draw_status_bar(void)
{
    akira_display_rect(0, SBAR_Y, SCR_W, SBAR_H, C_BLACK);

    /* Segmented battery: 5 segments at (4,7), outline 22x10 */
    int batt_pct = 0;
    for (int i = 0; g_batt_str[i] && g_batt_str[i] != '%'; i++) {
        if (g_batt_str[i] >= '0' && g_batt_str[i] <= '9') {
            batt_pct = batt_pct * 10 + (g_batt_str[i] - '0');
        }
    }
    if (batt_pct > 100) batt_pct = 100;

    akira_display_rect_outline(4, 7, 22, 10, C_WHITE);
    akira_display_rect(26, 10, 2, 4, C_WHITE);  /* nub */
    int segs = (batt_pct * 5 + 50) / 100;
    for (int s = 0; s < 5; s++) {
        akira_display_rect(6 + s * 4, 9, 3, 6, (s < segs) ? C_WHITE : C_BLACK);
    }
    akira_display_text(30, 7, g_batt_str, C_WHITE);

    /* WiFi + BT icons centred — dim if not connected */
    int cx = SCR_W / 2;
    draw_icon_nx(cx - 26, 4, 1, ICON_WIFI, g_wifi_conn ? C_WHITE : C_DKGRAY, C_BLACK);
    draw_icon_nx(cx + 6,  4, 1, ICON_BT,   g_bt_conn   ? C_WHITE : C_DKGRAY, C_BLACK);

    /* HH:MM:SS clock — right aligned */
    int tw = (int)strlen(g_time_str) * 8;
    akira_display_text(SCR_W - tw - 4, 7, g_time_str, C_WHITE);

    akira_display_hline(0, SBAR_H, SCR_W, C_WHITE);
}

/* ================================================================== */
/* Footer                                                              */
/* ================================================================== */

static void draw_footer(void)
{
    akira_display_hline(0, FOOT_Y - 1, SCR_W, C_WHITE);
    akira_display_rect(0, FOOT_Y, SCR_W, FOOT_H, C_BLACK);
    akira_display_text(8, FOOT_Y + 7, "A-Open", C_WHITE);
    const char *right = "B-Back";
    int rw = (int)strlen(right) * 8;
    akira_display_text(SCR_W - rw - 8, FOOT_Y + 7, right, C_WHITE);
}

/* ================================================================== */
/* Carousel                                                            */
/* ================================================================== */

#define CAR_SLOT_W      108  /* centre-to-adjacent spacing */
#define CAR_FOCUS_SCALE 4   /* 80x64 icon at full height */
#define CAR_ADJ_SCALE   2
#define CAR_ADJ_SINK    10  /* px: neighbours sink below focus baseline (perspective) */
#define CAR_LABEL_H     10

/*
 * draw_carousel — only 3 tiles visible: -1, 0, +1.
 * Focus scale shrinks automatically when car_h is compressed.
 * dimmed = true when options panel is open (no white focus bg).
 */
static void draw_carousel(int car_h, bool dimmed)
{
    akira_display_rect(0, CAR_Y, SCR_W, car_h, C_BLACK);

    if (g_total_tiles == 0) {
        draw_centred_text(0, CAR_Y + car_h / 2 - 5, SCR_W, "NO APPS", C_GRAY, C_BLACK);
        return;
    }

    /* Dynamic focus scale: fit icon + label + 2*pad inside car_h */
    int pad_f  = 10;
    int avail  = car_h - pad_f * 2 - 4;  /* label is outside the box */
    int fscale = avail / ICON_H;
    if (fscale > CAR_FOCUS_SCALE) fscale = CAR_FOCUS_SCALE;
    if (fscale < 1)               fscale = 1;
    int ascale = (fscale > 1) ? fscale - 1 : 1;
    if (ascale > CAR_ADJ_SCALE)   ascale = CAR_ADJ_SCALE;

    int centre_x = SCR_W / 2;
    int cy       = CAR_Y + car_h / 2;
    int focus_pop_y = anim_value(&g_anim_focus_pop);

    /* Draw adjacent tiles first so focus renders on top */
    for (int pass = 0; pass < 2; pass++) {
        for (int slot = -1; slot <= 1; slot++) {
            bool is_focus = (slot == 0);
            if (pass == 0 && is_focus)  continue;   /* focus on second pass */
            if (pass == 1 && !is_focus) continue;

            int idx   = ((g_sel + slot) % g_total_tiles + g_total_tiles) % g_total_tiles;
            int scale = is_focus ? fscale : ascale;
            int iw    = ICON_W * scale;
            int ih    = ICON_H * scale;
            int sx    = centre_x + slot * CAR_SLOT_W - iw / 2;
            int sy    = cy - ih / 2;
            if (!is_focus) sy += CAR_ADJ_SINK;  /* perspective: neighbours recede */
            if (is_focus)  sy += focus_pop_y;
            if (sy < CAR_Y) sy = CAR_Y;

            char upper[32] = {0};
            for (int i = 0; g_all_tiles[idx].name[i] && i < 31; i++) {
                upper[i] = (char)toupper((unsigned char)g_all_tiles[idx].name[i]);
            }

            if (is_focus && !dimmed) {
                /* Liquid Glass focus tile — icon box only, label below */
                int bx = sx - pad_f;
                int by = sy - pad_f;
                int bw = iw + pad_f * 2;
                int bh = ih + pad_f * 2;   /* icon box, no label inside */
                if (by < CAR_Y) by = CAR_Y;
                glass_rect_focus(bx, by, bw, bh, 6);
                draw_icon_nx(sx, by + pad_f, fscale, g_all_tiles[idx].icon, C_WHITE, C_BLACK);
                /* Label below the box */
                draw_centred_text(bx - 10, by + bh + 3, bw + 20, upper, C_WHITE, C_BLACK);
            } else {
                /* Adjacent or dimmed-focus tile */
                int pad_a = 5;
                int bx = sx - pad_a;
                int by = sy - pad_a;
                int bw = iw + pad_a * 2;
                int bh = ih + pad_a * 2;   /* icon box only */
                if (by < CAR_Y) by = CAR_Y;
                if (dimmed && is_focus) {
                    glass_rect(bx, by, bw, bh, 4);
                    draw_icon_nx(sx, by + pad_a, scale,
                                 g_all_tiles[idx].icon, C_GRAY, C_BLACK);
                    draw_centred_text(bx - 10, by + bh + 3, bw + 20, upper,
                                      C_GRAY, C_BLACK);
                } else {
                    glass_rect_dim(bx, by, bw, bh, 4);
                    draw_icon_nx(sx, by + pad_a, scale,
                                 g_all_tiles[idx].icon, C_DKGRAY, C_BLACK);
                    draw_centred_text(bx - 10, by + bh + 3, bw + 20, upper,
                                      C_DKGRAY, C_BLACK);
                }
            }
        }
    }

    /* Page dots removed */
}

/* ================================================================== */
/* Options panel                                                       */
/* ================================================================== */

static void draw_options_panel(int panel_y)
{
    if (panel_y >= SCR_H) return;

    const char **labels;
    if (g_sel >= 0 && g_sel < g_total_tiles &&
        g_all_tiles[g_sel].type == TILE_BUILTIN) {
        labels = s_opts_builtin;
    } else {
        labels = s_opts_wasm;
        s_opts_wasm[0] = (g_sel >= 0 && g_sel < g_total_tiles &&
                          g_all_tiles[g_sel].state == APP_STATE_RUNNING)
                         ? "STOP" : "START";
    }

    int ph = SCR_H - panel_y;

    /* Double hairline at panel top, black fill below */
    akira_display_hline(0, panel_y,     SCR_W, C_WHITE);
    akira_display_hline(0, panel_y + 1, SCR_W, C_WHITE);
    akira_display_rect(0, panel_y + OPT_HDR_H, SCR_W, ph - OPT_HDR_H, C_BLACK);

    /* Items */
    int items_y   = panel_y + OPT_HDR_H + 6;
    int content_h = SCR_H - items_y - 4;
    int vis       = content_h / OPT_ITEM_H;
    if (vis < 1) vis = 1;

    if (g_opts_scroll > g_opts_count - vis) g_opts_scroll = g_opts_count - vis;
    if (g_opts_scroll < 0)                  g_opts_scroll = 0;

    for (int i = g_opts_scroll; i < g_opts_count && i < g_opts_scroll + vis; i++) {
        int iy  = items_y + (i - g_opts_scroll) * OPT_ITEM_H;
        bool hi = (i == g_opts_focused);
        int  ty = iy + 3 + (OPT_ITEM_H - 6 - 10) / 2;

        if (hi) {
            glass_rect_focus(OPT_BTN_X, iy + 3, OPT_BTN_W, OPT_ITEM_H - 6, 5);
            draw_centred_text(OPT_BTN_X + OPT_TXT_PAD, ty,
                              OPT_BTN_W - OPT_TXT_PAD * 2,
                              labels[i], C_WHITE, C_GLASS_BODY);
        } else {
            glass_rect_dim(OPT_BTN_X, iy + 3, OPT_BTN_W, OPT_ITEM_H - 6, 5);
            draw_centred_text(OPT_BTN_X + OPT_TXT_PAD, ty,
                              OPT_BTN_W - OPT_TXT_PAD * 2,
                              labels[i], C_DKGRAY, C_GLASS_BODY);
        }
    }

    /* Scrollbar — only shown when content overflows */
    if (g_opts_count > vis) {
        int track_y = items_y;
        int track_h = vis * OPT_ITEM_H;
        akira_display_rect(OPT_SBAR_X, track_y, OPT_SBAR_W, track_h, C_BLACK);
        int thumb_h = track_h * vis / g_opts_count;
        if (thumb_h < 8) thumb_h = 8;
        int max_off = g_opts_count - vis;
        int thumb_y = track_y +
                      (track_h - thumb_h) * g_opts_scroll / max_off;
        akira_display_rect(OPT_SBAR_X, thumb_y, OPT_SBAR_W, thumb_h, C_WHITE);
    }
}

/* ================================================================== */
/* Full redraw                                                         */
/* ================================================================== */

static void full_redraw(void)
{
    int car_h   = anim_value(&g_anim_car_h);
    int panel_y = anim_value(&g_anim_panel_y);

    bool panel_visible = (panel_y < SCR_H);

    akira_display_clear(C_BLACK);
    draw_status_bar();
    draw_carousel(car_h, panel_visible);

    if (panel_visible) {
        draw_options_panel(panel_y);
    } else {
        draw_footer();
    }

    akira_display_flush();
    g_dirty = false;
}

/* ================================================================== */
/* Tile management                                                     */
/* ================================================================== */

static const uint32_t *name_to_icon(const char *name)
{
    char lc[48] = {0};
    for (int i = 0; name[i] && i < 47; i++) {
        lc[i] = (char)tolower((unsigned char)name[i]);
    }
    if (strstr(lc, "term")  || strstr(lc, "shell") || strstr(lc, "con"))  return ICON_TERMINAL;
    if (strstr(lc, "radio") || strstr(lc, "audio") || strstr(lc, "music")) return ICON_RADIO;
    if (strstr(lc, "cam")   || strstr(lc, "photo"))                         return ICON_CAMERA;
    if (strstr(lc, "game")  || strstr(lc, "retro") || strstr(lc, "play"))  return ICON_GAMES;
    if (strstr(lc, "set")   || strstr(lc, "cfg")   || strstr(lc, "tool"))  return ICON_SETTINGS;
    if (strstr(lc, "sd")    || strstr(lc, "file")  || strstr(lc, "stor"))  return ICON_SDCARD;
    return ICON_TERMINAL;
}

static void rebuild_tiles(void)
{
    app_info_t wasm[CONFIG_AKIRA_APP_MAX_INSTALLED];
    int n = app_manager_list(wasm, CONFIG_AKIRA_APP_MAX_INSTALLED);
    if (n < 0) n = 0;

    for (int i = 0; i < n; i++) {
        g_all_tiles[i].type   = TILE_WASM;
        strncpy(g_all_tiles[i].name, wasm[i].name, sizeof(g_all_tiles[i].name) - 1);
        g_all_tiles[i].name[sizeof(g_all_tiles[i].name) - 1] = '\0';
        g_all_tiles[i].icon   = name_to_icon(wasm[i].name);
        g_all_tiles[i].state  = wasm[i].state;
        g_all_tiles[i].action = NULL;
    }
    for (int i = 0; i < BUILTIN_COUNT; i++) {
        g_all_tiles[n + i] = BUILTINS[i];
    }
    g_total_tiles      = n + BUILTIN_COUNT;
    g_last_n_installed = n;

    if (g_total_tiles > 0 && g_sel >= g_total_tiles) {
        g_sel = g_total_tiles - 1;
    }
}

/* ================================================================== */
/* Options helpers                                                     */
/* ================================================================== */

static void opts_open(void)
{
    g_ui_state     = UI_OPTIONS;
    g_opts_focused = 0;
    g_opts_scroll  = 0;
    g_opts_count   = (g_sel >= 0 && g_sel < g_total_tiles &&
                      g_all_tiles[g_sel].type == TILE_BUILTIN)
                     ? OPTS_BUILTIN_COUNT : OPTS_WASM_COUNT;
    anim_start(&g_anim_car_h,   CAR_H_IDLE, CAR_H_OPT);
    anim_start(&g_anim_panel_y, SCR_H, OPT_PANEL_Y);
    g_dirty = true;
}

static void opts_close(void)
{
    g_ui_state = UI_HOME;
    anim_start(&g_anim_car_h,   CAR_H_OPT, CAR_H_IDLE);
    anim_start(&g_anim_panel_y, OPT_PANEL_Y, SCR_H);
    g_dirty = true;
}

static void opts_confirm(void)
{
    if (g_sel < 0 || g_sel >= g_total_tiles) {
        opts_close();
        return;
    }
    ui_tile_t *t = &g_all_tiles[g_sel];

    if (t->type == TILE_BUILTIN) {
        if (g_opts_focused == 0 && t->action) {
            opts_close();
            t->action();
            return;
        }
    } else {
        switch (g_opts_focused) {
        case 0:
            if (t->state == APP_STATE_RUNNING) {
                app_manager_stop(t->name);
            } else {
                app_manager_start(t->name);
            }
            break;
        case 1:
            app_manager_uninstall(t->name);
            break;
        default:
            break;
        }
    }
    opts_close();
}

/* ================================================================== */
/* Built-in actions                                                    */
/* ================================================================== */

static void action_settings(void)
{
    settings_screen_load();
}

static void action_sdcard(void)
{
    extern void sd_install_screen_load(void);
    sd_install_screen_load();
}

static void action_sysinfo(void)
{
    uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000U);
    char lines[5][56];
    snprintf(lines[0], 56, "AkiraOS %s", CONFIG_AKIRA_OS_VERSION);
    snprintf(lines[1], 56, "Board:  %s", CONFIG_BOARD);
    snprintf(lines[2], 56, "Uptime: %02u:%02u:%02u",
             uptime_s / 3600, (uptime_s % 3600) / 60, uptime_s % 60);
    snprintf(lines[3], 56, "Apps:   %d / %d",
             g_total_tiles - BUILTIN_COUNT, CONFIG_AKIRA_APP_MAX_INSTALLED);
    snprintf(lines[4], 56, "Built:  %s", __DATE__);

    int pw = 288, ph = 110;
    int px = (SCR_W - pw) / 2;
    int py = (SCR_H - ph) / 2;

    akira_display_rect(px, py, pw, ph, C_BLACK);
    akira_display_rect_outline(px,     py,     pw,     ph,     C_WHITE);
    akira_display_rect_outline(px + 1, py + 1, pw - 2, ph - 2, C_WHITE);
    draw_centred_text(px + 4, py + 6, pw - 8, "SYSTEM INFO", C_WHITE, C_BLACK);
    akira_display_hline(px + 4, py + 18, pw - 8, C_WHITE);

    for (int i = 0; i < 5; i++) {
        akira_display_text(px + 8, py + 22 + i * 16, lines[i], C_WHITE);
    }
    draw_centred_text(px + 4, py + ph - 14, pw - 8, "[B] CLOSE", C_WHITE, C_BLACK);
    akira_display_flush();

    while (true) {
        k_sleep(K_MSEC(50));
        uint32_t btns = akira_input_get_bitmask();
        if (btns & BIT(AKIRA_BTN_B)) {
            while (akira_input_get_bitmask() & BIT(AKIRA_BTN_B)) {
                k_sleep(K_MSEC(20));
            }
            break;
        }
    }
    g_dirty = true;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

void home_screen_create(void)
{
    g_total_tiles      = 0;
    g_sel              = 0;
    g_last_n_installed = -1;
    g_ui_state         = UI_HOME;
    g_opts_focused     = 0;
    g_opts_scroll      = 0;
    g_opts_count       = OPTS_WASM_COUNT;
    g_visible          = false;
    g_dirty            = false;
    memset(g_all_tiles, 0, sizeof(g_all_tiles));

    g_anim_car_h    = (anim_t){ CAR_H_IDLE, CAR_H_IDLE, ANIM_FRAMES };
    g_anim_panel_y  = (anim_t){ SCR_H,      SCR_H,      ANIM_FRAMES };
    g_anim_focus_pop = (anim_t){ 0, 0, ANIM_FRAMES };

    LOG_INF("Liquid Crystal home screen initialised");
}

void home_screen_refresh(void)
{
    rebuild_tiles();
    g_ui_state     = UI_HOME;
    g_anim_car_h    = (anim_t){ CAR_H_IDLE, CAR_H_IDLE, ANIM_FRAMES };
    g_anim_panel_y  = (anim_t){ SCR_H,      SCR_H,      ANIM_FRAMES };
    g_anim_focus_pop = (anim_t){ 0, 0, ANIM_FRAMES };
    g_visible      = true;
    g_dirty        = true;
    full_redraw();
}

void home_screen_load(void)
{
    g_visible = true;
    g_dirty   = true;
    full_redraw();
}

void home_screen_update_status(void)
{
    if (!g_visible) return;

    app_info_t tmp[CONFIG_AKIRA_APP_MAX_INSTALLED];
    int n = app_manager_list(tmp, CONFIG_AKIRA_APP_MAX_INSTALLED);
    if (n < 0) n = 0;
    if (n != g_last_n_installed) {
        rebuild_tiles();
        g_dirty = true;
    }

    int64_t epoch = akira_time_get_epoch();
    if (akira_time_is_set()) {
        int64_t day_sec = epoch % 86400;
        if (day_sec < 0) day_sec += 86400;
        snprintf(g_time_str, sizeof(g_time_str), "%02u:%02u:%02u",
                 (unsigned)(day_sec / 3600),
                 (unsigned)((day_sec % 3600) / 60),
                 (unsigned)(day_sec % 60));
    } else {
        uint32_t s = (uint32_t)epoch;
        snprintf(g_time_str, sizeof(g_time_str), "%02u:%02u:%02u",
                 (s / 3600U) % 24U, (s % 3600U) / 60U, s % 60U);
    }

    /* WiFi connectivity status */
#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
    {
        struct net_if *iface = net_if_get_default();
        struct wifi_iface_status wst = {0};
        g_wifi_conn = iface &&
                      net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface,
                               &wst, sizeof(wst)) == 0 &&
                      wst.state >= WIFI_STATE_ASSOCIATED;
    }
#endif
    /* BT connectivity status */
#ifdef CONFIG_BT
    {
        bt_state_t bts = bt_manager_get_state();
        g_bt_conn = (bts == BT_STATE_CONNECTED);
    }
#endif
}

void home_screen_tick(void)
{
    if (!g_visible) return;

    bool was = anim_running(&g_anim_car_h) || anim_running(&g_anim_panel_y) || anim_running(&g_anim_focus_pop);
    anim_step(&g_anim_car_h);
    anim_step(&g_anim_panel_y);
    anim_step(&g_anim_focus_pop);
    bool now = anim_running(&g_anim_car_h) || anim_running(&g_anim_panel_y) || anim_running(&g_anim_focus_pop);

    if (was || now || g_dirty) {
        full_redraw();
    }
}

void home_screen_handle_key(uint32_t just_pressed)
{
    if (!g_visible) return;

    if (g_ui_state == UI_OPTIONS) {
        if (just_pressed & BIT(AKIRA_BTN_UP)) {
            if (g_opts_focused > 0) {
                g_opts_focused--;
                if (g_opts_focused < g_opts_scroll) g_opts_scroll--;
                g_dirty = true;
            } else {
                /* Already at top — collapse panel back to carousel */
                opts_close();
            }
        }
        if (just_pressed & BIT(AKIRA_BTN_DOWN)) {
            if (g_opts_focused < g_opts_count - 1) {
                g_opts_focused++;
                int vis_h = SCR_H - OPT_PANEL_Y - OPT_HDR_H - 6;
                int vis   = vis_h / OPT_ITEM_H;
                if (vis < 1) vis = 1;
                if (g_opts_focused >= g_opts_scroll + vis) g_opts_scroll++;
            }
            g_dirty = true;
        }
        if (just_pressed & BIT(AKIRA_BTN_A)) {
            opts_confirm();
            /* Skip home_screen_refresh if a sub-screen took over display */
            if (!settings_screen_is_active()) {
                home_screen_refresh();
            }
            return;
        }
        if (just_pressed & BIT(AKIRA_BTN_B)) {
            opts_close();
        }
        return;
    }

    /* IDLE — carousel navigation */
    if (just_pressed & BIT(AKIRA_BTN_LEFT)) {
        g_sel = (g_sel - 1 + g_total_tiles) % g_total_tiles;
        anim_start_pop(&g_anim_focus_pop);
        g_dirty = true;
    }
    if (just_pressed & BIT(AKIRA_BTN_RIGHT)) {
        g_sel = (g_sel + 1) % g_total_tiles;
        anim_start_pop(&g_anim_focus_pop);
        g_dirty = true;
    }
    if ((just_pressed & BIT(AKIRA_BTN_DOWN)) ||
        (just_pressed & BIT(AKIRA_BTN_HOME))) {
        opts_open();
        return;
    }
    if (just_pressed & BIT(AKIRA_BTN_A)) {
        if (g_sel >= 0 && g_sel < g_total_tiles) {
            ui_tile_t *t = &g_all_tiles[g_sel];
            if (t->type == TILE_BUILTIN && t->action) {
                t->action();
            } else if (t->type == TILE_WASM) {
                app_manager_start(t->name);
            }
        }
    }
    /* B in idle = no-op (nothing to go back to on the root screen) */
}
