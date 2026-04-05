/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_home_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_home_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file home_screen.c
 * @brief AkiraConsole OS Shell — HOME screen.
 *
 * Pure akira_display_* renderer — no LVGL.
 *
 * Color palette note (ST7789V IPS panel, INVON disabled):
 *   Standard RGB565 — framebuffer values are displayed directly.
 *     C_BLACK = 0x0000  →  panel drives black  ✓
 *     C_WHITE = 0xFFFF  →  panel drives white  ✓
 *   Values match the WASM app SDK palette (akira_api.h).
 *
 * Layout (320×240):
 *   y=  0..31   Status bar: battery | WiFi/BT/AkiraNet | clock
 *   y= 32..33   Separator line
 *   y= 34..207  3×2 App grid (6 cells à 106×87 px)
 *   y=208..209  Separator line
 *   y=210..239  Bottom ribbon: [A] OPEN   [B] OPTIONS
 *
 * Selected cell is rendered inverted (white bg, black fg).
 * D-pad navigates.  A=launch/open  B=context-menu
 */

#include "home_screen.h"
#include "settings_screen.h"

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <runtime/app_manager/app_manager.h>
#include <lib/akira_time.h>

/* ------------------------------------------------------------------ */
/* Palette — standard RGB565 (matches akira_api.h COLOR_* constants) */
/* ------------------------------------------------------------------ */
#define C_BLACK  0x0000u   /* pure black                               */
#define C_WHITE  0xFFFFu   /* pure white                               */
#define C_GRAY   0x7BEFu   /* mid-gray  (COLOR_GRAY in akira_api.h)   */
#define C_DKGRAY 0x39E7u   /* dark gray (COLOR_DARK_GRAY)             */

/* ------------------------------------------------------------------ */
/* Screen geometry                                                     */
/* ------------------------------------------------------------------ */
#define SCR_W     320
#define SCR_H     240

#define SBAR_Y    0
#define SBAR_H    32

#define GRID_Y    34
#define GRID_H    174     /* 87 × 2 rows */
#define GRID_COLS 3
#define GRID_ROWS 2
#define CELL_W    (SCR_W / GRID_COLS)     /* 106 */
#define CELL_H    (GRID_H / GRID_ROWS)    /* 87  */
#define MAX_TILES (GRID_COLS * GRID_ROWS) /* 6   */

#define RIB_Y     210
#define RIB_H     30

/* ------------------------------------------------------------------ */
/* 1-bit pixel-art icons (20×16 px, uint32_t per row, bit31 = left)  */
/* ------------------------------------------------------------------ */
#define ICON_W  20
#define ICON_H  16

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

static const uint32_t ICON_MESH[ICON_H] = {
    0b00001111111111110000000000000000u,
    0b00110000000000011000000000000000u,
    0b01001111111111100100000000000000u,
    0b00010000000000010000000000000000u,
    0b00001111111111100000000000000000u,
    0b00000110000001100000000000000000u,
    0b00000001111110000000000000000000u,
    0b00000000110000000000000000000000u,
    0b00000001111000000000000000000000u,
    0b00000001111000000000000000000000u,
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

/* ------------------------------------------------------------------ */
/* Tile model                                                          */
/* ------------------------------------------------------------------ */
typedef enum { TILE_WASM, TILE_BUILTIN } tile_type_t;

typedef struct {
    tile_type_t    type;
    char           name[APP_NAME_MAX_LEN];
    const uint32_t *icon;
    /* WASM only */
    app_state_t    state;
    /* BUILTIN only */
    void           (*action)(void);
} ui_tile_t;

/* ------------------------------------------------------------------ */
/* Built-in action forward declarations                                */
/* ------------------------------------------------------------------ */
static void action_settings(void);
static void action_sdcard(void);
static void action_sysinfo(void);

/* ------------------------------------------------------------------ */
/* Built-in tile descriptors (always present at the end of the grid)  */
/* ------------------------------------------------------------------ */
#define BUILTIN_COUNT 3

static const ui_tile_t BUILTINS[BUILTIN_COUNT] = {
    { TILE_BUILTIN, "SETTINGS", ICON_SETTINGS, 0, action_settings },
    { TILE_BUILTIN, "SD CARD",  ICON_SDCARD,   0, action_sdcard   },
    { TILE_BUILTIN, "SYS INFO", ICON_INFO,     0, action_sysinfo  },
};

/* ------------------------------------------------------------------ */
/* Runtime state                                                       */
/* ------------------------------------------------------------------ */
/* Total capacity: all installable WASM apps + the 3 built-ins */
#define MAX_ALL_TILES  (CONFIG_AKIRA_APP_MAX_INSTALLED + BUILTIN_COUNT)

static ui_tile_t  g_all_tiles[MAX_ALL_TILES];
static int        g_total_tiles;      /* WASM installed + BUILTIN_COUNT */
static int        g_selected;         /* absolute tile index across all pages */
static int        g_last_n_installed; /* for auto-refresh after OTA install */
static bool       g_visible;

/* Context menu */
#define CTX_ITEMS_WASM    3
#define CTX_ITEMS_BUILTIN 2
#define CTX_ITEMS_MAX     3
static bool       g_ctx_open;
static int        g_ctx_focused;
static int        g_ctx_tile;    /* index into g_tiles[] */
static int        g_ctx_count;   /* actual number of ctx items */

/* Inline overlay (sysinfo / settings stub) */
typedef enum { OVL_NONE, OVL_SYSINFO, OVL_SETTINGS } overlay_t;
static overlay_t  g_overlay;

/* Uptime clock */
static char       g_time_str[8] = "00:00";
static char       g_batt_str[8] = "100%";

/* ------------------------------------------------------------------ */
/* Icon lookup from WASM app name                                      */
/* ------------------------------------------------------------------ */
static const uint32_t *name_to_icon(const char *name)
{
    char lc[48] = {0};
    for (int i = 0; name[i] && i < 47; i++) {
        lc[i] = (char)tolower((unsigned char)name[i]);
    }
    if (strstr(lc, "term")  || strstr(lc, "shell") || strstr(lc, "con")) {
        return ICON_TERMINAL;
    }
    if (strstr(lc, "radio") || strstr(lc, "audio") || strstr(lc, "music")) {
        return ICON_RADIO;
    }
    if (strstr(lc, "cam")   || strstr(lc, "photo")) {
        return ICON_CAMERA;
    }
    if (strstr(lc, "game")  || strstr(lc, "retro") || strstr(lc, "play")) {
        return ICON_GAMES;
    }
    if (strstr(lc, "set")   || strstr(lc, "cfg")   || strstr(lc, "tool")) {
        return ICON_SETTINGS;
    }
    if (strstr(lc, "sd")    || strstr(lc, "file")  || strstr(lc, "stor")) {
        return ICON_SDCARD;
    }
    return ICON_TERMINAL;
}

/* ------------------------------------------------------------------ */
/* Low-level draw helpers                                              */
/* ------------------------------------------------------------------ */
static void draw_icon_2x(int x, int y, const uint32_t *icon,
                         uint16_t fg, uint16_t bg)
{
    for (int r = 0; r < ICON_H; r++) {
        uint32_t bits = icon[r];
        for (int c = 0; c < ICON_W; c++) {
            uint16_t pc = (bits & (1u << (31 - c))) ? fg : bg;
            akira_display_pixel(x + c * 2,     y + r * 2,     pc);
            akira_display_pixel(x + c * 2 + 1, y + r * 2,     pc);
            akira_display_pixel(x + c * 2,     y + r * 2 + 1, pc);
            akira_display_pixel(x + c * 2 + 1, y + r * 2 + 1, pc);
        }
    }
}

static void draw_icon_1x(int x, int y, const uint32_t *icon,
                         uint16_t fg, uint16_t bg)
{
    for (int r = 0; r < ICON_H; r++) {
        uint32_t bits = icon[r];
        for (int c = 0; c < ICON_W; c++) {
            uint16_t pc = (bits & (1u << (31 - c))) ? fg : bg;
            akira_display_pixel(x + c, y + r, pc);
        }
    }
}

static void draw_centred_text(int x, int y, int w, const char *s,
                               uint16_t fg, uint16_t bg)
{
    int len = (int)strlen(s);
    int tw  = len * 8;
    int lx  = x + MAX(0, (w - tw) / 2);
    akira_display_rect(x, y, w, 10, bg);
    akira_display_text(lx, y, s, fg);
}

static void draw_right_text(int rx, int y, const char *s, uint16_t col)
{
    int tw = (int)strlen(s) * 8;
    akira_display_text(rx - tw, y, s, col);
}

/* ------------------------------------------------------------------ */
/* Status bar                                                          */
/* ------------------------------------------------------------------ */
static void draw_status_bar(void)
{
    akira_display_rect(0, SBAR_Y, SCR_W, SBAR_H, C_BLACK);

    /* Battery icon (left) */
    draw_icon_1x(4, 8, ICON_BATTERY, C_WHITE, C_BLACK);
    akira_display_text(26, 11, g_batt_str, C_WHITE);

    /* Three icons centred */
    int slot_x = 88;
    int slot_w = 48;

    draw_icon_1x(slot_x + 0 * slot_w + (slot_w - ICON_W) / 2, 6,
                 ICON_WIFI,  C_WHITE, C_BLACK);
    draw_centred_text(slot_x + 0 * slot_w, SBAR_H - 11, slot_w,
                      "WiFi", C_WHITE, C_BLACK);

    draw_icon_1x(slot_x + 1 * slot_w + (slot_w - ICON_W) / 2, 6,
                 ICON_BT,    C_WHITE, C_BLACK);
    draw_centred_text(slot_x + 1 * slot_w, SBAR_H - 11, slot_w,
                      "BT", C_WHITE, C_BLACK);

    draw_icon_1x(slot_x + 2 * slot_w + (slot_w - ICON_W) / 2, 6,
                 ICON_MESH,  C_WHITE, C_BLACK);
    draw_centred_text(slot_x + 2 * slot_w, SBAR_H - 11, slot_w,
                      "AkiraNet", C_WHITE, C_BLACK);

    /* Clock right */
    draw_right_text(SCR_W - 4, 11, g_time_str, C_WHITE);

    /* Separator */
    akira_display_hline(0, SBAR_H,     SCR_W, C_WHITE);
    akira_display_hline(0, SBAR_H + 1, SCR_W, C_BLACK);
}

/* ------------------------------------------------------------------ */
/* App grid cell                                                       */
/* ------------------------------------------------------------------ */
static void draw_cell(int local_idx)
{
    /* Map the page-local slot (0..MAX_TILES-1) to an absolute tile */
    int page    = g_selected / MAX_TILES;
    int abs_idx = page * MAX_TILES + local_idx;

    int col = local_idx % GRID_COLS;
    int row = local_idx / GRID_COLS;
    int cx  = col * CELL_W;
    int cy  = GRID_Y + row * CELL_H;

    bool sel = (abs_idx == g_selected);

    /* bg = selected → white, normal → black */
    uint16_t bg     = sel ? C_WHITE : C_BLACK;
    uint16_t fg     = sel ? C_BLACK : C_WHITE;
    uint16_t border = sel ? C_BLACK : C_GRAY;

    akira_display_rect(cx, cy, CELL_W, CELL_H, bg);

    if (abs_idx >= g_total_tiles) {
        /* Empty slot */
        akira_display_rect_outline(cx, cy, CELL_W, CELL_H, C_DKGRAY);
        return;
    }

    /* Border — 2px when selected */
    akira_display_rect_outline(cx, cy, CELL_W, CELL_H, border);
    if (sel) {
        akira_display_rect_outline(cx + 1, cy + 1, CELL_W - 2, CELL_H - 2, border);
        /* Pixel-art corner brackets */
        akira_display_hline(cx + 3, cy + 3, 7, C_WHITE);
        akira_display_vline(cx + 3, cy + 3, 7, C_WHITE);
        akira_display_hline(cx + CELL_W - 10, cy + 3, 7, C_WHITE);
        akira_display_vline(cx + CELL_W - 4,  cy + 3, 7, C_WHITE);
        akira_display_hline(cx + 3, cy + CELL_H - 4, 7, C_WHITE);
        akira_display_vline(cx + 3, cy + CELL_H - 10, 7, C_WHITE);
        akira_display_hline(cx + CELL_W - 10, cy + CELL_H - 4, 7, C_WHITE);
        akira_display_vline(cx + CELL_W - 4,  cy + CELL_H - 10, 7, C_WHITE);
    }

    /* Icon — centred, 2× scaled (40×32) */
    int icon_x = cx + (CELL_W - ICON_W * 2) / 2;
    int icon_y = cy + (CELL_H - ICON_H * 2 - 14) / 2;
    draw_icon_2x(icon_x, icon_y, g_all_tiles[abs_idx].icon, fg, bg);

    /* Name label */
    char upper[32] = {0};
    for (int i = 0; g_all_tiles[abs_idx].name[i] && i < 31; i++) {
        upper[i] = (char)toupper((unsigned char)g_all_tiles[abs_idx].name[i]);
    }
    draw_centred_text(cx + 2, cy + CELL_H - 14, CELL_W - 4, upper, fg, bg);
}

static void draw_grid(void)
{
    /* Horizontal separator between rows */
    akira_display_hline(0, GRID_Y + CELL_H, SCR_W, C_GRAY);
    /* Vertical separators */
    for (int c = 1; c < GRID_COLS; c++) {
        akira_display_vline(c * CELL_W, GRID_Y, GRID_H, C_GRAY);
    }
    for (int i = 0; i < MAX_TILES; i++) {
        draw_cell(i);
    }
}

/* ------------------------------------------------------------------ */
/* Bottom ribbon                                                       */
/* ------------------------------------------------------------------ */
static void draw_ribbon(void)
{
    int pages = (g_total_tiles + MAX_TILES - 1) / MAX_TILES;
    if (pages < 1) pages = 1;

    akira_display_hline(0, RIB_Y - 1, SCR_W, C_WHITE);
    akira_display_rect(0, RIB_Y, SCR_W, RIB_H, C_BLACK);
    akira_display_text(6,  RIB_Y + 10, "[A] OPEN",   C_WHITE);
    draw_right_text(SCR_W - 6, RIB_Y + 10, "[B] OPTS", C_WHITE);

    if (pages > 1) {
        /* Show page indicator + X/Y page-flip hint */
        char page_str[16];
        int cur_pg = g_selected / MAX_TILES;
        snprintf(page_str, sizeof(page_str), "[X] %d/%d [Y]", cur_pg + 1, pages);
        draw_centred_text(SCR_W / 2 - 36, RIB_Y + 10, 72, page_str, C_WHITE, C_BLACK);
    }
}

/* ------------------------------------------------------------------ */
/* Context menu                                                        */
/* ------------------------------------------------------------------ */
#define CTX_W  200
#define CTX_H  110
#define CTX_X  ((SCR_W - CTX_W) / 2)
#define CTX_Y  ((SCR_H - CTX_H) / 2)
#define CTX_BTN_H 22

static const char *s_ctx_wasm[CTX_ITEMS_WASM]       = {"START",  "UNINSTALL", "CANCEL"};
static const char *s_ctx_builtin[CTX_ITEMS_BUILTIN]  = {"OPEN",   "CANCEL"};

static void draw_context_menu(void)
{
    const char **labels;
    if (g_ctx_tile >= 0 && g_ctx_tile < g_total_tiles &&
        g_all_tiles[g_ctx_tile].type == TILE_BUILTIN) {
        labels = s_ctx_builtin;
    } else {
        labels = s_ctx_wasm;
        /* If app is running, rename START → STOP */
        if (g_ctx_tile >= 0 && g_ctx_tile < g_total_tiles &&
            g_all_tiles[g_ctx_tile].state == APP_STATE_RUNNING) {
            s_ctx_wasm[0] = "STOP";
        } else {
            s_ctx_wasm[0] = "START";
        }
    }

    akira_display_rect(CTX_X, CTX_Y, CTX_W, CTX_H, C_BLACK);
    akira_display_rect_outline(CTX_X,     CTX_Y,     CTX_W,     CTX_H,     C_WHITE);
    akira_display_rect_outline(CTX_X + 2, CTX_Y + 2, CTX_W - 4, CTX_H - 4, C_WHITE);

    /* Title */
    char title[36] = "[ OPTIONS ]";
    if (g_ctx_tile >= 0 && g_ctx_tile < g_total_tiles) {
        snprintf(title, sizeof(title), "[ %s ]", g_all_tiles[g_ctx_tile].name);
    }
    draw_centred_text(CTX_X + 4, CTX_Y + 6, CTX_W - 8, title, C_WHITE, C_BLACK);
    akira_display_hline(CTX_X + 4, CTX_Y + 18, CTX_W - 8, C_WHITE);

    for (int i = 0; i < g_ctx_count; i++) {
        int by  = CTX_Y + 22 + i * (CTX_BTN_H + 2);
        bool sel = (i == g_ctx_focused);
        uint16_t bbg = sel ? C_WHITE : C_BLACK;
        uint16_t bfg = sel ? C_BLACK : C_WHITE;
        akira_display_rect(CTX_X + 6, by, CTX_W - 12, CTX_BTN_H, bbg);
        if (!sel) {
            akira_display_rect_outline(CTX_X + 6, by, CTX_W - 12, CTX_BTN_H, C_WHITE);
        }
        draw_centred_text(CTX_X + 6, by + 6, CTX_W - 12, labels[i], bfg, bbg);
    }
}

static void ctx_confirm(void)
{
    if (g_ctx_tile < 0 || g_ctx_tile >= g_total_tiles) {
        g_ctx_open = false;
        return;
    }
    ui_tile_t *t = &g_all_tiles[g_ctx_tile];

    if (t->type == TILE_BUILTIN) {
        /* 0=OPEN, 1=CANCEL */
        if (g_ctx_focused == 0 && t->action) {
            t->action();
        }
    } else {
        /* 0=START/STOP, 1=UNINSTALL, 2=CANCEL */
        switch (g_ctx_focused) {
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
    g_ctx_open = false;
}

/* ------------------------------------------------------------------ */
/* Inline overlay screens                                              */
/* ------------------------------------------------------------------ */
static void draw_overlay_sysinfo(void)
{
    uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000U);
    char lines[6][48];

    snprintf(lines[0], 48, "AkiraOS %s", CONFIG_AKIRA_OS_VERSION);
    snprintf(lines[1], 48, "Platform: %s", CONFIG_BOARD);
    snprintf(lines[2], 48, "Uptime:   %02u:%02u:%02u",
             uptime_s / 3600, (uptime_s % 3600) / 60, uptime_s % 60);
    snprintf(lines[3], 48, "Build:    %s", __DATE__);
    snprintf(lines[4], 48, "WASM apps (max %d)", CONFIG_AKIRA_APP_MAX_INSTALLED);
    snprintf(lines[5], 48, "[B] CLOSE");

    /* Centred panel 280×120 */
    int px = (SCR_W - 280) / 2, py = (SCR_H - 130) / 2;
    akira_display_rect(px, py, 280, 130, C_BLACK);
    akira_display_rect_outline(px,     py,     280,     130,     C_WHITE);
    akira_display_rect_outline(px + 2, py + 2, 276, 126, C_WHITE);

    draw_centred_text(px + 4, py + 6, 272, "SYSTEM INFO", C_WHITE, C_BLACK);
    akira_display_hline(px + 4, py + 18, 272, C_WHITE);

    for (int i = 0; i < 5; i++) {
        akira_display_text(px + 8, py + 22 + i * 16, lines[i], C_WHITE);
    }
    draw_centred_text(px + 4, py + 107, 272, lines[5], C_WHITE, C_BLACK);
}

static void draw_overlay_settings_stub(void)
{
    int px = (SCR_W - 280) / 2, py = (SCR_H - 80) / 2;
    akira_display_rect(px, py, 280, 80, C_BLACK);
    akira_display_rect_outline(px,     py,     280, 80, C_WHITE);
    akira_display_rect_outline(px + 2, py + 2, 276, 76, C_WHITE);
    draw_centred_text(px + 4, py + 6,  272, "SETTINGS", C_WHITE, C_BLACK);
    akira_display_hline(px + 4, py + 18, 272, C_WHITE);
    akira_display_text(px + 8, py + 28, "Settings screen coming soon.", C_WHITE);
    akira_display_text(px + 8, py + 44, "Flash via OTA or BLE OTA.", C_WHITE);
    draw_centred_text(px + 4, py + 60, 272, "[B] CLOSE", C_WHITE, C_BLACK);
}

/* ------------------------------------------------------------------ */
/* Built-in actions                                                    */
/* ------------------------------------------------------------------ */
static void action_settings(void)
{
    g_overlay = OVL_NONE;
    settings_screen_load();
}

static void action_sdcard(void)
{
    /* SD card is LVGL-based; show a simple notice */
    g_overlay = OVL_SYSINFO; /* re-use overlay mode so [B] closes */
    int px = (SCR_W - 280) / 2, py = (SCR_H - 80) / 2;
    akira_display_rect(px, py, 280, 80, C_BLACK);
    akira_display_rect_outline(px,     py,     280, 80, C_WHITE);
    akira_display_rect_outline(px + 2, py + 2, 276, 76, C_WHITE);
    draw_centred_text(px + 4, py + 6,  272, "SD CARD", C_WHITE, C_BLACK);
    akira_display_hline(px + 4, py + 18, 272, C_WHITE);
    akira_display_text(px + 8, py + 28, "Insert SD card with /apps/ folder.", C_WHITE);
    akira_display_text(px + 8, py + 44, "Install via BLE or OTA web UI.", C_WHITE);
    draw_centred_text(px + 4, py + 60, 272, "[B] CLOSE", C_WHITE, C_BLACK);
    akira_display_flush();
}

static void action_sysinfo(void)
{
    g_overlay = OVL_SYSINFO;
    akira_display_clear(C_BLACK);
    draw_status_bar();
    draw_grid();
    draw_ribbon();
    draw_overlay_sysinfo();
    akira_display_flush();
}

/* ------------------------------------------------------------------ */
/* Full redraw                                                         */
/* ------------------------------------------------------------------ */
static void full_redraw(void)
{
    akira_display_clear(C_BLACK);
    draw_status_bar();
    draw_grid();
    draw_ribbon();
    if (g_ctx_open) {
        draw_context_menu();
    } else if (g_overlay != OVL_NONE) {
        if (g_overlay == OVL_SYSINFO) {
            draw_overlay_sysinfo();
        }
    }
    akira_display_flush();
}

/* ------------------------------------------------------------------ */
/* Build the tile list: WASM apps first, then built-ins               */
/* ------------------------------------------------------------------ */
static void rebuild_tiles(void)
{
    app_info_t wasm[CONFIG_AKIRA_APP_MAX_INSTALLED];
    int n = app_manager_list(wasm, CONFIG_AKIRA_APP_MAX_INSTALLED);

    if (n < 0) {
        n = 0;
    }

    /* Fill WASM tiles */
    for (int i = 0; i < n; i++) {
        g_all_tiles[i].type   = TILE_WASM;
        strncpy(g_all_tiles[i].name, wasm[i].name, sizeof(g_all_tiles[i].name) - 1);
        g_all_tiles[i].name[sizeof(g_all_tiles[i].name) - 1] = '\0';
        g_all_tiles[i].icon   = name_to_icon(wasm[i].name);
        g_all_tiles[i].state  = wasm[i].state;
        g_all_tiles[i].action = NULL;
    }

    /* Append built-ins after all WASM tiles */
    for (int i = 0; i < BUILTIN_COUNT; i++) {
        g_all_tiles[n + i] = BUILTINS[i];
    }

    g_total_tiles      = n + BUILTIN_COUNT;
    g_last_n_installed = n;

    /* Clamp selection to valid range */
    if (g_selected >= g_total_tiles) {
        g_selected = g_total_tiles - 1;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void home_screen_create(void)
{
    g_total_tiles      = 0;
    g_selected         = 0;
    g_last_n_installed = -1;
    g_ctx_open         = false;
    g_ctx_focused      = 0;
    g_ctx_tile         = 0;
    g_ctx_count        = CTX_ITEMS_WASM;
    g_overlay          = OVL_NONE;
    g_visible          = false;
    memset(g_all_tiles, 0, sizeof(g_all_tiles));
    LOG_INF("HOME screen created (no-LVGL renderer, INVON palette)");
}

void home_screen_refresh(void)
{
    rebuild_tiles();
    g_ctx_open  = false;
    g_overlay   = OVL_NONE;
    g_visible   = true;
    full_redraw();
}

void home_screen_load(void)
{
    g_visible = true;
    full_redraw();
}

void home_screen_update_status(void)
{
    if (!g_visible) {
        return;
    }

    /* Auto-refresh: detect newly installed / uninstalled apps each second */
    {
        app_info_t tmp[CONFIG_AKIRA_APP_MAX_INSTALLED];
        int n = app_manager_list(tmp, CONFIG_AKIRA_APP_MAX_INSTALLED);
        if (n < 0) n = 0;
        if (n != g_last_n_installed) {
            rebuild_tiles();
            full_redraw();   /* full_redraw calls akira_display_flush */
            return;
        }
    }

    /* Update clock: use real time if set, otherwise show uptime */
    int64_t epoch = akira_time_get_epoch();
    if (akira_time_is_set()) {
        /* Real wall-clock HH:MM (UTC) */
        int64_t day_sec = epoch % 86400;
        if (day_sec < 0) day_sec += 86400;
        snprintf(g_time_str, sizeof(g_time_str), "%02u:%02u",
                 (unsigned)(day_sec / 3600),
                 (unsigned)((day_sec % 3600) / 60));
    } else {
        uint32_t s = (uint32_t)(epoch);
        snprintf(g_time_str, sizeof(g_time_str), "%02u:%02u",
                 (s / 3600U) % 24U, (s % 3600U) / 60U);
    }

    draw_status_bar();
    akira_display_flush();
}

void home_screen_handle_key(uint32_t just_pressed)
{
    if (!g_visible) {
        return;
    }

    /* Overlay is active — [B] closes it */
    if (g_overlay != OVL_NONE) {
        if (just_pressed & BIT(AKIRA_BTN_B)) {
            g_overlay = OVL_NONE;
            full_redraw();
        }
        return;
    }

    /* Context menu has priority */
    if (g_ctx_open) {
        bool changed = false;
        if (just_pressed & BIT(AKIRA_BTN_UP)) {
            g_ctx_focused = (g_ctx_focused - 1 + g_ctx_count) % g_ctx_count;
            changed = true;
        }
        if (just_pressed & BIT(AKIRA_BTN_DOWN)) {
            g_ctx_focused = (g_ctx_focused + 1) % g_ctx_count;
            changed = true;
        }
        if (just_pressed & BIT(AKIRA_BTN_A)) {
            ctx_confirm();
            home_screen_refresh();
            return;
        }
        if (just_pressed & BIT(AKIRA_BTN_B)) {
            g_ctx_open = false;
            changed = true;
        }
        if (changed) {
            full_redraw();
        }
        return;
    }

    /* Grid navigation — g_selected is absolute; page = g_selected / MAX_TILES */
    int prev       = g_selected;
    int pg         = g_selected / MAX_TILES;
    int local      = g_selected % MAX_TILES;
    int lrow       = local / GRID_COLS;
    int lcol       = local % GRID_COLS;
    bool pg_changed = false;
    int total_pgs  = (g_total_tiles + MAX_TILES - 1) / MAX_TILES;
    if (total_pgs < 1) total_pgs = 1;

    if (just_pressed & BIT(AKIRA_BTN_RIGHT)) {
        if (lcol < GRID_COLS - 1) {
            int nxt = g_selected + 1;
            if (nxt / MAX_TILES == pg && nxt < g_total_tiles)
                g_selected = nxt;
        }
        ARG_UNUSED(lrow);
    }
    if (just_pressed & BIT(AKIRA_BTN_LEFT)) {
        if (lcol > 0) {
            int nxt = g_selected - 1;
            if (nxt >= 0 && nxt / MAX_TILES == pg)
                g_selected = nxt;
        }
    }
    if (just_pressed & BIT(AKIRA_BTN_DOWN)) {
        int nxt = g_selected + GRID_COLS;
        if (nxt / MAX_TILES == pg && nxt < g_total_tiles)
            g_selected = nxt;
    }
    if (just_pressed & BIT(AKIRA_BTN_UP)) {
        int nxt = g_selected - GRID_COLS;
        if (nxt >= 0 && nxt / MAX_TILES == pg)
            g_selected = nxt;
    }

    /* [X] prev page  /  [Y] next page */
    if ((just_pressed & BIT(AKIRA_BTN_X)) && pg > 0) {
        g_selected  = (pg - 1) * MAX_TILES;
        pg_changed  = true;
    }
    if ((just_pressed & BIT(AKIRA_BTN_Y)) && pg < total_pgs - 1) {
        int first = (pg + 1) * MAX_TILES;
        g_selected = (first < g_total_tiles) ? first : g_total_tiles - 1;
        pg_changed  = true;
    }

    if (pg_changed) {
        full_redraw();
    } else if (g_selected != prev) {
        /* Fast path: redraw only the two changed cells */
        draw_cell(prev % MAX_TILES);
        draw_cell(g_selected % MAX_TILES);
        akira_display_flush();
    }

    /* [A] = launch / open */
    if (just_pressed & BIT(AKIRA_BTN_A)) {
        ui_tile_t *t = &g_all_tiles[g_selected];
        if (t->type == TILE_BUILTIN && t->action) {
            t->action();
        } else if (t->type == TILE_WASM) {
            int r = app_manager_start(t->name);
            if (r < 0) {
                LOG_ERR("Start '%s': %d", t->name, r);
            }
        }
    }

    /* [B] = context menu */
    if (just_pressed & BIT(AKIRA_BTN_B)) {
        g_ctx_open    = true;
        g_ctx_focused = 0;
        g_ctx_tile    = g_selected;
        g_ctx_count   = (g_all_tiles[g_selected].type == TILE_BUILTIN)
                        ? CTX_ITEMS_BUILTIN : CTX_ITEMS_WASM;
        full_redraw();
    }
}
