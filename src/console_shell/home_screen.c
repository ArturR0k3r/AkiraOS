/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_home_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_home_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file home_screen.c
 * @brief AkiraOS home launcher — the root of the shell's screen stack.
 *
 * Vertical carousel of Playdate-style dither cards + a slide-up options panel.
 *
 * Layout (320x240 landscape):
 *   y=  0..25   status bar (screen framework)
 *   y= 26..215  carousel band: focused card centred, neighbours above/below
 *   y=216..239  footer hints (screen framework) — or the options panel,
 *               which slides up over the lower part of the band
 *
 * Moving the selection slides the whole stack one slot and pops the focused
 * card; opening options shrinks the band while the panel rises.  All motion
 * is time-based (ui/anim.h), so it runs at the same speed however fast the
 * shell manages to draw.
 */

#include "home_screen.h"
#include "akiraplay_screen.h"
#include "sd_install_screen.h"
#include "settings_screen.h"
#include "shell_theme.h"
#include "status_model.h"
#include "akira_os_shell.h"
#include "ui/akira_ui.h"
#include "ui/anim.h"
#include "ui/overlay.h"
#include "ui/screen.h"

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <runtime/app_manager/app_manager.h>

/* ---- Geometry ------------------------------------------------------ */
#define CAR_Y 26
#define CAR_H_IDLE (FOOT_Y - CAR_Y)  /* 190 */
#define CAR_H_OPT 88                 /* carousel height when options open */

/* options panel top when fully open */
#define OPT_PANEL_Y (CAR_Y + CAR_H_OPT) /* 114 */
#define OPT_ITEM_H 32

/* options panel button geometry (leave right strip for scrollbar) */
#define OPT_BTN_X 8
#define OPT_BTN_W (SCR_W - 30)
#define OPT_SBAR_X (SCR_W - 14)
#define OPT_SBAR_W 7

/* carousel cards */
#define CARD_SEL_H 58
#define CARD_ADJ_H 40
#define CARD_GAP 8
#define CARD_STRIDE (CARD_ADJ_H + CARD_GAP)

/* ---- Motion -------------------------------------------------------- */
#define PANEL_MS 260
#define SLIDE_MS 200
#define POP_MS 220
#define POP_PX 8

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
static const uint32_t ICON_STORE[ICON_H] __maybe_unused = {
    0b00000000111100000000000000000000u,
    0b00000001000010000000000000000000u,
    0b00000001000010000000000000000000u,
    0b00011111111111111000000000000000u,
    0b00010000000000001000000000000000u,
    0b00010000000000001000000000000000u,
    0b00010001111110001000000000000000u,
    0b00010000000000001000000000000000u,
    0b00010000000000001000000000000000u,
    0b00010000000000001000000000000000u,
    0b00010000000000001000000000000000u,
    0b00011111111111111000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
};
static const uint32_t ICON_SETTINGS[ICON_H] = {
    0b00000001110000000000000000000000u,
    0b00000001110000000000000000000000u,
    0b00001111111110000000000000000000u,
    0b00011000000001100000000000000000u,
    0b00010000111000100000000000000000u,
    0b00001111111110000000000000000000u,
    0b00000001110000000000000000000000u,
    0b00000001110000000000000000000000u,
    0b00000011111000000000000000000000u,
    0b00000110001100000000000000000000u,
    0b00001110001110000000000000000000u,
    0b00000110001100000000000000000000u,
    0b00000011111000000000000000000000u,
    0b00000001110000000000000000000000u,
    0b00000001110000000000000000000000u,
    0b00000000000000000000000000000000u,
};
static const uint32_t ICON_SDCARD[ICON_H] = {
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
    0b01111110000000000000000000000000u,
    0b01111111111111111110000000000000u,
    0b01000000000000000010000000000000u,
    0b01000111001001110010000000000000u,
    0b01001000101101000010000000000000u,
    0b01000111001101000010000000000000u,
    0b01000000101001000010000000000000u,
    0b01000111001001110010000000000000u,
    0b01000000000000000010000000000000u,
    0b01000000000000000010000000000000u,
    0b01000000000000000010000000000000u,
    0b01111111111111111110000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
};
static const uint32_t ICON_INFO[ICON_H] = {
    0b00000000000000000000000000000000u,
    0b00000011111100000000000000000000u,
    0b00001110000111000000000000000000u,
    0b00011000000001100000000000000000u,
    0b00010000110000100000000000000000u,
    0b00100000110000010000000000000000u,
    0b00100000000000010000000000000000u,
    0b00100000110000010000000000000000u,
    0b00100000110000010000000000000000u,
    0b00100000110000010000000000000000u,
    0b00010001111000100000000000000000u,
    0b00011000000001100000000000000000u,
    0b00001110000111000000000000000000u,
    0b00000011111100000000000000000000u,
    0b00000000000000000000000000000000u,
    0b00000000000000000000000000000000u,
};

/* ---- Tile model -------------------------------------------------- */
typedef enum
{
    TILE_WASM,
    TILE_BUILTIN
} tile_type_t;

typedef struct
{
    tile_type_t type;
    char name[APP_NAME_MAX_LEN];
    const char *blurb;
    const uint32_t *icon;
    app_state_t state;
    void (*action)(void);
} ui_tile_t;

static void action_sysinfo(void);

static const ui_tile_t BUILTINS[] = {
    {TILE_BUILTIN, "SETTINGS", "Wi-Fi, display, power", ICON_SETTINGS, 0, settings_open},
    {TILE_BUILTIN, "SD CARD", "Install apps from SD", ICON_SDCARD, 0, sd_install_open},
    {TILE_BUILTIN, "SYS INFO", "About this console", ICON_INFO, 0, action_sysinfo},
#if defined(CONFIG_AKIRA_WIFI_MANAGER)
    {TILE_BUILTIN, "AKIRA PLAY", "Browse the app store", ICON_STORE, 0, akiraplay_open},
#endif
};
#define BUILTIN_COUNT ARRAY_SIZE(BUILTINS)

#define MAX_ALL_TILES (CONFIG_AKIRA_APP_MAX_INSTALLED + BUILTIN_COUNT)
static ui_tile_t g_all_tiles[MAX_ALL_TILES] __attribute__((section(".ext_ram.bss")));
static int g_total_tiles;
static int g_last_n_installed = -1;

/* ---- Carousel state --------------------------------------------- */
static int g_sel;

/* ---- Options panel state ---------------------------------------- */
static bool g_opts_open;
static int g_opts_focused;
static int g_opts_scroll;
static int g_opts_count;

#define OPTS_WASM_COUNT 3
#define OPTS_BUILTIN_COUNT 2
static const char *s_opts_wasm[OPTS_WASM_COUNT] = {"START", "UNINSTALL", "CANCEL"};
static const char *s_opts_builtin[OPTS_BUILTIN_COUNT] = {"OPEN", "CANCEL"};

/* ---- Animation state -------------------------------------------- */
static anim_t g_anim_car_h;   /* carousel band height */
static anim_t g_anim_panel_y; /* options panel top */
static anim_t g_anim_slide;   /* stack offset after a selection move (px → 0) */
static anim_t g_anim_pop;     /* focused card drop (px → 0) */

/* ================================================================== */
/* Draw helpers                                                        */
/* ================================================================== */

static void draw_centred_text(int x, int y, int w, const char *s, uint16_t fg)
{
    char clipped[64];
    int max_chars = MAX(w / 8, 1);
    int slen = (int)strlen(s);

    if (slen > max_chars)
    {
        int keep = max_chars > 2 ? max_chars - 2 : max_chars;
        snprintf(clipped, sizeof(clipped), "%.*s%s", keep, s, max_chars > 2 ? ".." : "");
        s = clipped;
    }
    int tw = (int)strlen(s) * 8;
    akira_display_text(x + (tw < w ? (w - tw) / 2 : 0), y, s, fg);
}

/* ================================================================== */
/* Carousel                                                            */
/* ================================================================== */

/*
 * draw_carousel — the focused tile centred in the band, neighbours stacked
 * above and below.  Cards may extend past the band edges mid-slide: the
 * status bar, the footer and the options panel are all painted afterwards
 * and mask them.
 */
static void draw_carousel(int car_h)
{
    if (g_total_tiles == 0)
    {
        draw_centred_text(0, CAR_Y + car_h / 2 - 5, SCR_W, "NO APPS", C_GRAY);
        return;
    }

    int cx = SCR_W / 2;
    int cy = CAR_Y + car_h / 2;
    int slide = anim_value(&g_anim_slide);
    int pop = anim_value(&g_anim_pop);
    const int card_w = SCR_W - 52;

    /* Draw neighbours first, focus last so its shadow sits on top. */
    for (int pass = 0; pass < 2; pass++)
    {
        for (int slot = -3; slot <= 3; slot++)
        {
            bool is_focus = (slot == 0);
            if ((pass == 0) == is_focus)
                continue;
            if (g_total_tiles < 2 && !is_focus)
                continue;

            int idx = ((g_sel + slot) % g_total_tiles + g_total_tiles) % g_total_tiles;
            int ch = is_focus ? CARD_SEL_H : CARD_ADJ_H;
            int cw = is_focus ? card_w : card_w - 28;
            int cardx = cx - cw / 2 - 6;
            /* Neighbours sit a selected-card's half-height away from centre. */
            int base = (slot == 0) ? -ch / 2
                     : (slot < 0) ? -CARD_SEL_H / 2 - CARD_GAP + slot * CARD_STRIDE + CARD_GAP
                                  : CARD_SEL_H / 2 + CARD_GAP + (slot - 1) * CARD_STRIDE;
            int cardy = cy + base + slide + (is_focus ? pop : 0);

            if (cardy > CAR_Y + car_h || cardy + ch < CAR_Y - 4)
                continue;

            akira_ui_dither_card(cardx, cardy, cw, ch, 12, is_focus, is_focus ? 5 : 2);

            uint16_t fg = is_focus ? C_BLACK : C_WHITE; /* content on the card */
            uint16_t bg = is_focus ? C_WHITE : C_BLACK;

            int isc = is_focus ? 2 : 1;
            int iw = ICON_W * isc, ih = ICON_H * isc;
            akira_ui_icon_1bpp(cardx + 16, cardy + (ch - ih) / 2, isc,
                               g_all_tiles[idx].icon, ICON_W, ICON_H, fg, bg,
                               AKIRA_UI_TRANSPARENT);

            char upper[32] = {0};
            for (int i = 0; g_all_tiles[idx].name[i] && i < 31; i++)
            {
                upper[i] = (char)toupper((unsigned char)g_all_tiles[idx].name[i]);
            }
            int tx = cardx + 16 + iw + 14;
            if (is_focus)
            {
                const char *blurb = g_all_tiles[idx].blurb
                                        ? g_all_tiles[idx].blurb
                                        : (g_all_tiles[idx].state == APP_STATE_RUNNING
                                               ? "Running"
                                               : "Installed app");
                akira_display_text(tx, cardy + ch / 2 - 12, upper, fg);
                akira_display_text(tx, cardy + ch / 2 + 3, blurb, SHELL_C_DKGRAY);
            }
            else
            {
                akira_display_text(tx, cardy + (ch - 10) / 2, upper, fg);
            }
        }
    }

    /* Position dots down the right edge — where am I in the list. */
    if (g_total_tiles > 1 && g_total_tiles <= 16)
    {
        int dot_gap = 9;
        int top = cy - (g_total_tiles - 1) * dot_gap / 2;
        for (int i = 0; i < g_total_tiles; i++)
        {
            int dy = top + i * dot_gap;
            if (i == g_sel)
                akira_display_rounded_rect_fill(SCR_W - 13, dy - 4, 6, 8, 3, C_WHITE);
            else
                akira_display_circle_fill(SCR_W - 10, dy, 1, C_GRAY);
        }
    }
}

/* ================================================================== */
/* Options panel                                                       */
/* ================================================================== */

static const char **opts_labels(void)
{
    if (g_sel >= 0 && g_sel < g_total_tiles && g_all_tiles[g_sel].type == TILE_BUILTIN)
    {
        return s_opts_builtin;
    }
    s_opts_wasm[0] = (g_sel >= 0 && g_sel < g_total_tiles &&
                      g_all_tiles[g_sel].state == APP_STATE_RUNNING)
                         ? "STOP"
                         : "START";
    return s_opts_wasm;
}

static int opts_visible(void)
{
    /* The panel ends where the footer hint bar begins. */
    return MAX((FOOT_Y - OPT_PANEL_Y - 4) / OPT_ITEM_H, 1);
}

static void draw_options_panel(int panel_y)
{
    if (panel_y >= SCR_H)
        return;

    const char **labels = opts_labels();
    int vis = opts_visible();

    /* Panel: background up from panel_y with a dotted rule on top. */
    akira_display_rect(0, panel_y, SCR_W, SCR_H - panel_y, C_BLACK);
    for (int x = 0; x < SCR_W; x += 2)
        akira_display_pixel(x, panel_y, C_WHITE);

    int items_y = panel_y + 4;

    for (int i = g_opts_scroll; i < g_opts_count && i < g_opts_scroll + vis; i++)
    {
        int iy = items_y + (i - g_opts_scroll) * OPT_ITEM_H;
        bool hi = (i == g_opts_focused);

        akira_ui_dither_card(OPT_BTN_X, iy + 2, OPT_BTN_W, OPT_ITEM_H - 6, 8, hi, hi ? 3 : 2);
        draw_centred_text(OPT_BTN_X + 10, iy + 2 + (OPT_ITEM_H - 6 - 10) / 2,
                          OPT_BTN_W - 20, labels[i], hi ? C_BLACK : C_WHITE);
    }

    /* Scrollbar — only shown when content overflows */
    if (g_opts_count > vis)
    {
        int track_h = vis * OPT_ITEM_H;
        int thumb_h = MAX(track_h * vis / g_opts_count, 8);
        int thumb_y = items_y + (track_h - thumb_h) * g_opts_scroll / (g_opts_count - vis);

        akira_display_vline(OPT_SBAR_X + OPT_SBAR_W / 2, items_y, track_h, C_DKGRAY);
        akira_display_rounded_rect_fill(OPT_SBAR_X, thumb_y, OPT_SBAR_W, thumb_h, 3, C_WHITE);
    }
}

static void home_draw(void)
{
    draw_carousel(anim_value(&g_anim_car_h));
    draw_options_panel(anim_value(&g_anim_panel_y));
}

/* ================================================================== */
/* Tile management                                                     */
/* ================================================================== */

static const uint32_t *name_to_icon(const char *name)
{
    char lc[48] = {0};
    for (int i = 0; name[i] && i < 47; i++)
    {
        lc[i] = (char)tolower((unsigned char)name[i]);
    }
    if (strstr(lc, "term") || strstr(lc, "shell") || strstr(lc, "con"))
        return ICON_TERMINAL;
    if (strstr(lc, "radio") || strstr(lc, "audio") || strstr(lc, "music"))
        return ICON_RADIO;
    if (strstr(lc, "cam") || strstr(lc, "photo"))
        return ICON_CAMERA;
    if (strstr(lc, "game") || strstr(lc, "retro") || strstr(lc, "play"))
        return ICON_GAMES;
    if (strstr(lc, "set") || strstr(lc, "cfg") || strstr(lc, "tool"))
        return ICON_SETTINGS;
    if (strstr(lc, "sd") || strstr(lc, "file") || strstr(lc, "stor"))
        return ICON_SDCARD;
    return ICON_TERMINAL;
}

static void rebuild_tiles(void)
{
    app_info_t wasm[CONFIG_AKIRA_APP_MAX_INSTALLED];
    int n = app_manager_list(wasm, CONFIG_AKIRA_APP_MAX_INSTALLED);
    if (n < 0)
        n = 0;

    for (int i = 0; i < n; i++)
    {
        g_all_tiles[i].type = TILE_WASM;
        strncpy(g_all_tiles[i].name, wasm[i].name, sizeof(g_all_tiles[i].name) - 1);
        g_all_tiles[i].name[sizeof(g_all_tiles[i].name) - 1] = '\0';
        g_all_tiles[i].blurb = NULL;
        g_all_tiles[i].icon = name_to_icon(wasm[i].name);
        g_all_tiles[i].state = wasm[i].state;
        g_all_tiles[i].action = NULL;
    }
    for (int i = 0; i < (int)BUILTIN_COUNT; i++)
    {
        g_all_tiles[n + i] = BUILTINS[i];
    }
    g_total_tiles = n + BUILTIN_COUNT;
    g_last_n_installed = n;

    if (g_total_tiles > 0 && g_sel >= g_total_tiles)
    {
        g_sel = g_total_tiles - 1;
    }
}

/* ================================================================== */
/* Options panel behaviour                                             */
/* ================================================================== */

static void opts_open(void)
{
    g_opts_open = true;
    g_opts_focused = 0;
    g_opts_scroll = 0;
    g_opts_count = (g_sel >= 0 && g_sel < g_total_tiles &&
                    g_all_tiles[g_sel].type == TILE_BUILTIN)
                       ? OPTS_BUILTIN_COUNT
                       : OPTS_WASM_COUNT;
    anim_to(&g_anim_car_h, CAR_H_OPT, PANEL_MS, ANIM_EASE_INOUT);
    anim_to(&g_anim_panel_y, OPT_PANEL_Y, PANEL_MS, ANIM_EASE_INOUT);
    screen_set_hints("A:Select", "B:Close");
}

static void opts_close(void)
{
    g_opts_open = false;
    anim_to(&g_anim_car_h, CAR_H_IDLE, PANEL_MS, ANIM_EASE_INOUT);
    anim_to(&g_anim_panel_y, SCR_H, PANEL_MS, ANIM_EASE_INOUT);
    screen_set_hints("A:Open", "HOME:Options");
}

static void launch_app(const char *name)
{
    akira_shell_set_wasm_launching();
    if (app_manager_start(name) < 0)
    {
        akira_shell_abort_wasm_launch();
        overlay_toast(TOAST_ICON_ERROR, "Couldn't start app", name, 2500);
        home_screen_refresh();
    }
}

static void uninstall_confirmed(bool confirmed, void *ctx)
{
    ARG_UNUSED(ctx);
    if (!confirmed || g_sel < 0 || g_sel >= g_total_tiles)
        return;

    char name[APP_NAME_MAX_LEN];
    snprintf(name, sizeof(name), "%s", g_all_tiles[g_sel].name);
    if (app_manager_uninstall(name) == 0)
        overlay_toast(TOAST_ICON_OK, "Uninstalled", name, 2000);
    else
        overlay_toast(TOAST_ICON_ERROR, "Uninstall failed", name, 2500);
    home_screen_refresh();
}

static void opts_confirm(void)
{
    if (g_sel < 0 || g_sel >= g_total_tiles)
    {
        opts_close();
        return;
    }
    ui_tile_t *t = &g_all_tiles[g_sel];
    int choice = g_opts_focused;

    opts_close();
    if (t->type == TILE_BUILTIN)
    {
        if (choice == 0 && t->action)
            t->action();
        return;
    }

    switch (choice)
    {
    case 0:
        if (t->state == APP_STATE_RUNNING)
        {
            app_manager_stop(t->name);
            home_screen_refresh();
        }
        else
        {
            launch_app(t->name);
        }
        break;
    case 1:
    {
        static char q[48];
        snprintf(q, sizeof(q), "Uninstall %s?", t->name);
        akira_ui_confirm_open("APP_UNINSTALL", q, NULL, uninstall_confirmed, NULL);
        break;
    }
    default:
        break;
    }
}

/* ================================================================== */
/* Screen callbacks                                                    */
/* ================================================================== */

static void home_key(uint32_t just_pressed)
{
    if (g_opts_open)
    {
        if (just_pressed & BIT(AKIRA_BTN_UP))
        {
            if (g_opts_focused > 0)
            {
                g_opts_focused--;
                if (g_opts_focused < g_opts_scroll)
                    g_opts_scroll--;
                screen_invalidate();
            }
            else
            {
                /* Already at top — collapse panel back to carousel */
                opts_close();
            }
        }
        if (just_pressed & BIT(AKIRA_BTN_DOWN))
        {
            if (g_opts_focused < g_opts_count - 1)
            {
                g_opts_focused++;
                if (g_opts_focused >= g_opts_scroll + opts_visible())
                    g_opts_scroll++;
                screen_invalidate();
            }
        }
        if (just_pressed & BIT(AKIRA_BTN_A))
        {
            opts_confirm();
            return;
        }
        if (just_pressed & (BIT(AKIRA_BTN_B) | BIT(AKIRA_BTN_HOME)))
        {
            opts_close();
        }
        return;
    }

    if (g_total_tiles == 0)
        return;

    /* Vertical carousel navigation: UP = previous app, DOWN = next. */
    if (just_pressed & (BIT(AKIRA_BTN_UP) | BIT(AKIRA_BTN_DOWN)))
    {
        int dir = (just_pressed & BIT(AKIRA_BTN_UP)) ? -1 : 1;

        g_sel = (g_sel + dir + g_total_tiles) % g_total_tiles;
        /* Start the stack one slot back and let it glide into place. */
        anim_start(&g_anim_slide, dir * CARD_STRIDE, 0, SLIDE_MS, ANIM_EASE_OUT);
        anim_start(&g_anim_pop, POP_PX, 0, POP_MS, ANIM_EASE_OUT_BACK);
    }
    if (just_pressed & (BIT(AKIRA_BTN_HOME) | BIT(AKIRA_BTN_X)))
    {
        opts_open();
        return;
    }
    if (just_pressed & BIT(AKIRA_BTN_A))
    {
        ui_tile_t *t = &g_all_tiles[g_sel];
        if (t->type == TILE_BUILTIN && t->action)
        {
            t->action();
        }
        else if (t->type == TILE_WASM)
        {
            launch_app(t->name);
        }
    }
    /* B on the root screen: nothing to go back to. */
}

static bool home_tick(void)
{
    return anim_running(&g_anim_car_h) || anim_running(&g_anim_panel_y) ||
           anim_running(&g_anim_slide) || anim_running(&g_anim_pop);
}

static void home_status(void)
{
    app_info_t tmp[CONFIG_AKIRA_APP_MAX_INSTALLED];
    int n = app_manager_list(tmp, CONFIG_AKIRA_APP_MAX_INSTALLED);

    if (MAX(n, 0) != g_last_n_installed)
    {
        rebuild_tiles();
        screen_invalidate();
    }
}

static void home_enter(void *arg)
{
    ARG_UNUSED(arg);
    g_sel = 0;
    g_opts_open = false;
    anim_set(&g_anim_car_h, CAR_H_IDLE);
    anim_set(&g_anim_panel_y, SCR_H);
    anim_set(&g_anim_slide, 0);
    anim_set(&g_anim_pop, 0);
    rebuild_tiles();
}

static void home_resume(void)
{
    /* Back from a sub-screen: it may have installed or removed apps. */
    rebuild_tiles();
}

const shell_screen_t home_screen = {
    .name = "home",
    .title = NULL,
    .flags = SCR_F_STATUSBAR | SCR_F_FOOTER,
    .hint_l = "A:Open",
    .hint_r = "HOME:Options",
    .enter = home_enter,
    .resume = home_resume,
    .on_key = home_key,
    .tick = home_tick,
    .draw = home_draw,
    .on_status = home_status,
};

void home_screen_refresh(void)
{
    rebuild_tiles();
    screen_invalidate();
}

/* ================================================================== */
/* System info (modal)                                                 */
/* ================================================================== */

static void sysinfo_draw(void)
{
    const shell_status_t *st = status_model_get();
    uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000U);
    char lines[6][48];

    snprintf(lines[0], sizeof(lines[0]), "AkiraOS  v%s", CONFIG_AKIRA_OS_VERSION);
    snprintf(lines[1], sizeof(lines[1]), "Board    %s", CONFIG_BOARD);
    snprintf(lines[2], sizeof(lines[2]), "Uptime   %02u:%02u:%02u",
             uptime_s / 3600, (uptime_s % 3600) / 60, uptime_s % 60);
    snprintf(lines[3], sizeof(lines[3]), "Apps     %d / %d",
             g_total_tiles - (int)BUILTIN_COUNT, CONFIG_AKIRA_APP_MAX_INSTALLED);
    if (st->batt_valid)
        snprintf(lines[4], sizeof(lines[4]), "Battery  %u%%  %d mV", st->batt_pct,
                 (int)st->batt_mv);
    else
        snprintf(lines[4], sizeof(lines[4]), "Battery  --");
    snprintf(lines[5], sizeof(lines[5]), "Built    %s", __DATE__);

    int pw = SCR_W - 48, ph = 156;
    int px = (SCR_W - pw) / 2, py = (SCR_H - ph) / 2 + 4;

    akira_ui_dither_card(px, py, pw, ph, 14, false, 5);
    akira_display_text(px + 16, py + 12, "SYSTEM INFO", C_WHITE);
    for (int x = px + 16; x < px + pw - 16; x += 2)
        akira_display_pixel(x, py + 27, C_GRAY);
    for (int i = 0; i < 6; i++)
        akira_display_text(px + 16, py + 36 + i * 17, lines[i], C_WHITE);
}

static void sysinfo_key(uint32_t just)
{
    if (just & (BIT(AKIRA_BTN_A) | BIT(AKIRA_BTN_B)))
        screen_pop();
}

static void sysinfo_status(void)
{
    screen_invalidate(); /* uptime ticks */
}

static const shell_screen_t sysinfo_screen = {
    .name = "sysinfo",
    .flags = SCR_F_MODAL,
    .on_key = sysinfo_key,
    .draw = sysinfo_draw,
    .on_status = sysinfo_status,
};

static void action_sysinfo(void)
{
    screen_push(&sysinfo_screen, NULL);
}
