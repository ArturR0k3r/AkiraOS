/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_settings
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_shell_settings, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file settings_screen.c
 * @brief AkiraConsole Settings — WiFi connect/disconnect, Web Server
 *        start/stop, About.  Pure akira_display_* renderer, no LVGL.
 *
 * Color palette (standard RGB565, INVON disabled — same as home_screen.c):
 *   C_BLACK = 0x0000  →  displayed black
 *   C_WHITE = 0xFFFF  →  displayed white
 *
 * Layout (320×240):
 *   y=  0..31   Title header bar
 *   y= 32..33   Separator
 *   y= 34..207  Content area (174 px)
 *   y=208..209  Separator
 *   y=210..239  Bottom ribbon (button hints)
 */

#include "settings_screen.h"
#include "home_screen.h"
#include "shell_theme.h"
#include "ui/akira_ui.h"
#include "settings/datetime_screen.h"
#include "settings/display_screen.h"
#include "settings/power_screen.h"
#include "settings/apps_screen.h"
#include "settings/ota_screen.h"
#include "settings/devmode_screen.h"
#include "settings/wifi_screen.h"

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <drivers/platform_hal.h>
#include <zephyr/version.h>

#ifdef CONFIG_BT
#include <connectivity/bluetooth/bt_manager.h>
#if defined(CONFIG_AKIRA_BT_COMPANION)
#include <settings/settings.h>
#include <zephyr/sys/reboot.h>
#endif
#endif

/* ------------------------------------------------------------------ */
/* Palette                                                            */
/* ------------------------------------------------------------------ */
#define C_BLACK 0x0000u
#define C_WHITE 0xFFFFu
#define C_GRAY 0x7BEFu
#define C_DKGRAY 0x39E7u
#define C_GLASS_HILIT 0x7BEFu
#define C_GLASS_BODY 0x0000u

/* ------------------------------------------------------------------ */
/* Geometry — width from CONFIG_AKIRA_OS_SHELL_SCREEN_W (shell_theme.h) */
/* ------------------------------------------------------------------ */
#define SCR_H 240
#define SBAR_H 24  /* status bar height */
#define CONT_Y 26  /* content area top  */
#define CONT_H 188 /* 240-24-2-26 = content to ribbon */
#define RIB_Y 216
#define RIB_H 24

#define MENU_X 16
#define MENU_W (SCR_W - 32) /* 288 */
#define MENU_ITH 32         /* menu item height */

/* ------------------------------------------------------------------ */
/* Page state machine                                                  */
/* ------------------------------------------------------------------ */
typedef enum
{
    SS_MAIN = 0,
    SS_BLUETOOTH,
    SS_ABOUT,
    SS_SLEEP,
} ss_page_t;

static bool g_active;
static ss_page_t g_page;

/* ---- Main menu --------------------------------------------------- */
typedef enum
{
    MAIN_ITEM_WIFI = 0,
    MAIN_ITEM_BLUETOOTH,
    MAIN_ITEM_DATETIME,
    MAIN_ITEM_DISPLAY,
    MAIN_ITEM_POWER,
    MAIN_ITEM_APPS,
    MAIN_ITEM_OTA,
    MAIN_ITEM_DEVELOPER,
    MAIN_ITEM_ABOUT,
    MAIN_ITEM_SLEEP,
    MAIN_ITEMS,
} main_item_t;

static const char *s_main_labels[MAIN_ITEMS] = {
    "WiFi", "Bluetooth",
    "Date & Time", "Display", "Power",
    "Apps", "OTA Update", "Developer",
    "About", "Sleep"};
static int g_main_sel;

/* ---- Bluetooth menu (4 items) ------------------------------------ */
#if defined(CONFIG_AKIRA_BT_COMPANION)
#define BT_ITEMS 5
/* Label text is refreshed in draw_bluetooth() to show the current boot mode. */
static char s_bt_mode_label[24] = "BLE Mode: HID";
static const char *s_bt_labels[BT_ITEMS] = {
    "Adv ON", "Adv OFF", "Unpair All", s_bt_mode_label, "Back"};
#else
#define BT_ITEMS 4
static const char *s_bt_labels[BT_ITEMS] = {
    "Adv ON", "Adv OFF", "Unpair All", "Back"};
#endif
static int g_bt_sel;

/* ---- Scroll offsets (index of first visible item per menu) ------- */
static int g_main_scroll;
static int g_bt_scroll;

/* ---- Sleep state ------------------------------------------------- */
static bool g_sleeping;
/* Timestamp of first B press during sleep (ms); -1 = no pending press */
static int64_t g_sleep_first_press_ms;
#define SLEEP_DPRESSW_MS 500 /* double-press window in ms */
/* Set by settings_screen_load() to absorb the B press that came from a
 * sub-screen's blocking loop before settings re-enables its own handler. */
static bool g_flush_next_key;

/* ------------------------------------------------------------------ */
/* Draw helpers                                                        */
/* ------------------------------------------------------------------ */
static void glass_rect(int x, int y, int w, int h, int r)
{
    akira_display_rounded_rect_fill(x, y, w, h, r, C_BLACK);
    akira_display_rounded_rect(x, y, w, h, r, C_WHITE);
    if (w > 4 && h > 4)
    {
        int ri = (r > 1) ? r - 1 : 0;
        akira_display_rounded_rect(x + 1, y + 1, w - 2, h - 2, ri, C_DKGRAY);
    }
    int ti = r + 2;
    if (w > ti * 2 && h > 6)
        akira_display_hline(x + ti, y + 3, w - ti * 2, C_GLASS_HILIT);
    if (w > 12 && h > 7)
    {
        int gw = w / 5;
        if (gw > 14)
            gw = 14;
        akira_display_rect(x + ti, y + 3, gw, 2, C_WHITE);
        if (gw > 4)
            akira_display_hline(x + ti, y + 5, gw / 2, C_GLASS_HILIT);
    }
    if (w > ti * 2 && h > 8)
        akira_display_hline(x + ti, y + h - 4, w - ti * 2, C_DKGRAY);
}

static void glass_rect_focus(int x, int y, int w, int h, int r)
{
    akira_display_rounded_rect_fill(x, y, w, h, r, C_BLACK);
    akira_display_rounded_rect(x, y, w, h, r, C_WHITE);
    if (w > 2 && h > 2)
    {
        int r1 = (r > 0) ? r - 1 : 0;
        akira_display_rounded_rect(x + 1, y + 1, w - 2, h - 2, r1, C_WHITE);
    }
    if (w > 6 && h > 6)
    {
        int r2 = (r > 1) ? r - 2 : 0;
        akira_display_rounded_rect(x + 2, y + 2, w - 4, h - 4, r2, C_DKGRAY);
    }
    int ti = r + 3;
    if (w > ti * 2 && h > 8)
        akira_display_hline(x + ti, y + 4, w - ti * 2, C_GLASS_HILIT);
    if (w > 14 && h > 9)
    {
        int gw = w / 5;
        if (gw > 16)
            gw = 16;
        akira_display_rect(x + ti, y + 4, gw, 2, C_WHITE);
        if (gw > 4)
            akira_display_hline(x + ti, y + 6, gw / 2, C_GLASS_HILIT);
    }
    if (w > ti * 2 && h > 10)
        akira_display_hline(x + ti, y + h - 5, w - ti * 2, C_DKGRAY);
    if (w > 14 && h > 11)
    {
        int gw = w / 6;
        if (gw > 12)
            gw = 12;
        akira_display_hline(x + w - ti - gw, y + h - 5, gw, C_WHITE);
        akira_display_hline(x + w - ti - gw, y + h - 4, gw / 2, C_GLASS_HILIT);
    }
}

static void glass_rect_dim(int x, int y, int w, int h, int r)
{
    akira_display_rounded_rect_fill(x, y, w, h, r, C_BLACK);
    akira_display_rounded_rect(x, y, w, h, r, C_DKGRAY);
    if (w > 4 && h > 4)
    {
        int ri = (r > 1) ? r - 1 : 0;
        akira_display_rounded_rect(x + 1, y + 1, w - 2, h - 2, ri, C_BLACK);
    }
    int ti = r + 2;
    if (w > ti * 2 && h > 6)
        akira_display_hline(x + ti, y + 3, w - ti * 2, C_DKGRAY);
    if (w > 12 && h > 7)
    {
        int gw = w / 5;
        if (gw > 14)
            gw = 14;
        akira_display_hline(x + ti, y + 3, gw, C_GRAY);
    }
    if (w > ti * 2 && h > 8)
        akira_display_hline(x + ti, y + h - 4, w - ti * 2, C_BLACK);
}

static void draw_centred(int x, int y, int w, const char *s,
                         uint16_t fg, uint16_t bg)
{
    int len = (int)strlen(s);
    int tw = len * 8;
    int lx = x + (tw < w ? (w - tw) / 2 : 0);
    akira_display_rect(x, y, w, 10, bg);
    akira_display_text(lx, y, s, fg);
}

static void draw_right(int rx, int y, const char *s, uint16_t col)
{
    akira_display_text(rx - (int)strlen(s) * 8, y, s, col);
}

static void draw_header(const char *title)
{
    /* Shared chrome: the kit's one inverted top bar (title left-aligned). */
    akira_ui_status_t sb = {
        .title = title,
        .clock = NULL,
        .battery_pct = -1,
        .show_wifi = false,
        .show_bt = false,
    };
    akira_ui_status_bar(&sb);
}

static void draw_ribbon(const char *left, const char *right)
{
    akira_display_hline(0, RIB_Y - 1, SCR_W, C_WHITE);
    akira_display_hline(0, RIB_Y - 2, SCR_W, C_WHITE);
    akira_display_rect(0, RIB_Y, SCR_W, RIB_H, C_BLACK);
    if (left && *left)
        akira_display_text(8, RIB_Y + 7, left, C_WHITE);
    if (right && *right)
        draw_right(SCR_W - 8, RIB_Y + 7, right, C_WHITE);
}

/* How many MENU_ITH items fit between top_y and the ribbon */
static int menu_vis_count(int top_y)
{
    return (RIB_Y - 1 - top_y) / MENU_ITH;
}

/* Adjust scroll so that sel is always visible; returns new scroll */
static int scroll_clamp(int sel, int scroll, int count, int top_y)
{
    int vis = menu_vis_count(top_y);
    if (sel < scroll)
        return sel;
    if (sel >= scroll + vis)
        return sel - vis + 1;
    return scroll;
}

/* Render a vertical scrollable list of menu items inside top_y .. RIB_Y-1.
 * scroll = index of the first visible item. */
static void draw_menu_at(const char **labels, int count, int sel,
                         int top_y, int scroll)
{
    int vis = menu_vis_count(top_y);

    /* Clear the entire menu + indicator area */
    akira_display_rect(0, top_y, SCR_W, RIB_Y - top_y, C_BLACK);

    for (int i = scroll; i < count && i < scroll + vis; i++)
    {
        int row = i - scroll;
        int iy = top_y + row * MENU_ITH;
        bool hi = (i == sel);
        int bx = MENU_X, by = iy + 2, bw = MENU_W, bh = MENU_ITH - 4;

        akira_ui_dither_card(bx, by, bw, bh, 8, hi, hi ? 3 : 2);
        int ty = by + (bh - 10) / 2;
        uint16_t fg = hi ? C_BLACK : C_WHITE;
        uint16_t cbg = hi ? C_WHITE : C_BLACK;
        draw_centred(bx + 4, ty, bw - 28, labels[i], fg, cbg);
        akira_display_text(bx + bw - 18, ty, ">", fg);
    }

    /* Scrollbar — only when content overflows */
    if (count > vis)
    {
        int sbar_x = SCR_W - 8;
        int sbar_w = 5;
        int track_y = top_y + 2;
        int track_h = vis * MENU_ITH - 4;
        akira_display_rect(sbar_x, track_y, sbar_w, track_h, C_BLACK);
        int thumb_h = track_h * vis / count;
        if (thumb_h < 8)
            thumb_h = 8;
        int max_off = count - vis;
        int thumb_y = track_y + (track_h - thumb_h) * scroll / max_off;
        akira_display_rect(sbar_x + 1, thumb_y, sbar_w - 2, thumb_h, C_WHITE);
    }
}

/* ------------------------------------------------------------------ */
/* Shared draw helpers — non-static, used by settings sub-screens     */
/* ------------------------------------------------------------------ */
/* Playdate dither-shadow cards: focus = filled + elevated, dim = idle. Callers
 * must draw selected text in INK (C_BLACK) and idle text in PAPER (C_WHITE). */
void ss_glass_rect_focus(int x, int y, int w, int h, int r) { (void)r; akira_ui_dither_card(x, y, w, h, 8, true, 3); }
void ss_glass_rect_dim(int x, int y, int w, int h, int r) { (void)r; akira_ui_dither_card(x, y, w, h, 8, false, 2); }
void ss_draw_centred(int x, int y, int w, const char *s, uint16_t fg, uint16_t bg) { draw_centred(x, y, w, s, fg, bg); }
void ss_draw_header(const char *title) { draw_header(title); }
void ss_draw_ribbon(const char *left, const char *right) { draw_ribbon(left, right); }
int ss_menu_vis_count(int top_y) { return menu_vis_count(top_y); }
int ss_scroll_clamp(int sel, int scroll, int count, int top_y) { return scroll_clamp(sel, scroll, count, top_y); }
void ss_draw_menu_at(const char **labels, int count, int sel, int top_y, int scroll) { draw_menu_at(labels, count, sel, top_y, scroll); }

/* ------------------------------------------------------------------ */
/* Page renderers                                                      */
/* ------------------------------------------------------------------ */
static void draw_main(void)
{
    draw_header("SETTINGS");
    akira_display_rect(0, CONT_Y, SCR_W, CONT_H, C_BLACK);
    draw_menu_at(s_main_labels, MAIN_ITEMS, g_main_sel, CONT_Y + 15, g_main_scroll);
    draw_ribbon("[A] SELECT", "[B] HOME");
}

static void draw_about(void)
{
    draw_header("About AkiraOS");
    akira_display_rect(0, CONT_Y, SCR_W, CONT_H, C_BLACK);

    char lines[5][48];
    snprintf(lines[0], 48, "AkiraOS  v%s", CONFIG_AKIRA_OS_VERSION);
    snprintf(lines[1], 48, "Board:   %s", CONFIG_BOARD);
    snprintf(lines[2], 48, "Built:   %s", __DATE__);
    snprintf(lines[4], 48, "WASM micro runtime embedded");

    int sy = CONT_Y + (CONT_H - 5 * 22) / 2;
    for (int i = 0; i < 5; i++)
    {
        akira_display_text(16, sy + i * 22, lines[i], C_WHITE);
    }
    draw_ribbon("", "[B] BACK");
}

static void draw_bluetooth(void)
{
    draw_header("Bluetooth");
    akira_display_rect(0, CONT_Y, SCR_W, CONT_H, C_BLACK);

    char state_str[20] = "OFF";
    char addr_str[20] = "---";

#ifdef CONFIG_BT
    bt_state_t bts = bt_manager_get_state();
    if (bts == BT_STATE_ADVERTISING)
        strncpy(state_str, "ADVERTISING", sizeof(state_str) - 1);
    else if (bts == BT_STATE_CONNECTED)
        strncpy(state_str, "CONNECTED", sizeof(state_str) - 1);
    else if (bts == BT_STATE_READY)
        strncpy(state_str, "READY", sizeof(state_str) - 1);
    else if (bts == BT_STATE_PAIRING)
        strncpy(state_str, "PAIRING", sizeof(state_str) - 1);
    else if (bts == BT_STATE_INITIALIZING)
        strncpy(state_str, "INIT", sizeof(state_str) - 1);
    bt_manager_get_address(addr_str, sizeof(addr_str));
#endif

    char l1[48], l2[48];
    snprintf(l1, sizeof(l1), "State: %s", state_str);
    snprintf(l2, sizeof(l2), "Addr:  %s", addr_str);

    akira_display_rounded_rect_fill(4, CONT_Y + 4, SCR_W - 8, 34, 3, C_BLACK);
    akira_display_rounded_rect(4, CONT_Y + 4, SCR_W - 8, 34, 3, C_DKGRAY);
    akira_display_text(10, CONT_Y + 10, l1, C_WHITE);
    akira_display_text(10, CONT_Y + 24, l2, C_WHITE);

#if defined(CONFIG_AKIRA_BT_COMPANION)
    snprintf(s_bt_mode_label, sizeof(s_bt_mode_label), "BLE Mode: %s",
             bt_manager_boot_mode_is_companion() ? "Companion" : "HID");
#endif

    draw_menu_at(s_bt_labels, BT_ITEMS, g_bt_sel, CONT_Y + 44, g_bt_scroll);
    draw_ribbon("[A] SELECT", "[B] BACK");
}

static void redraw(void)
{
    akira_display_clear(C_BLACK);
    switch (g_page)
    {
    case SS_MAIN:
        draw_main();
        break;
    case SS_BLUETOOTH:
        draw_bluetooth();
        break;
    case SS_ABOUT:
        draw_about();
        break;
    case SS_SLEEP: /* nothing — display is blanked */
        break;
    default:
        break;
    }
    akira_display_flush();
}

/* ------------------------------------------------------------------ */
/* Key handlers                                                        */
/* ------------------------------------------------------------------ */
static void handle_main(uint32_t k)
{
    bool ch = false;
    if (k & BIT(AKIRA_BTN_UP))
    {
        if (g_main_sel > 0)
        {
            g_main_sel--;
            g_main_scroll = scroll_clamp(g_main_sel, g_main_scroll, MAIN_ITEMS, CONT_Y + 15);
            ch = true;
        }
    }
    if (k & BIT(AKIRA_BTN_DOWN))
    {
        if (g_main_sel < MAIN_ITEMS - 1)
        {
            g_main_sel++;
            g_main_scroll = scroll_clamp(g_main_sel, g_main_scroll, MAIN_ITEMS, CONT_Y + 15);
            ch = true;
        }
    }
    if (k & BIT(AKIRA_BTN_A))
    {
        switch (g_main_sel)
        {
        case MAIN_ITEM_WIFI:
            g_active = false;
#ifdef CONFIG_AKIRA_WIFI_MANAGER
            wifi_screen_load();
#endif
            return;
        case MAIN_ITEM_BLUETOOTH:
            g_bt_sel = 0;
            g_bt_scroll = 0;
            g_page = SS_BLUETOOTH;
            break;
        case MAIN_ITEM_DATETIME:
            /* Date & Time — hands off to datetime_screen (blocking loop) */
            g_active = false;
            datetime_screen_load();
            return;
        case MAIN_ITEM_DISPLAY:
            g_active = false;
            display_screen_load();
            return;
        case MAIN_ITEM_POWER:
            g_active = false;
            power_screen_load();
            return;
        case MAIN_ITEM_APPS:
            g_active = false;
            apps_screen_load();
            return;
        case MAIN_ITEM_OTA:
            g_active = false;
            ota_screen_load();
            return;
        case MAIN_ITEM_DEVELOPER:
            g_active = false;
            devmode_screen_load();
            return;
        case MAIN_ITEM_ABOUT:
            g_page = SS_ABOUT;
            break;
        case MAIN_ITEM_SLEEP:
            g_sleeping = true;
            g_sleep_first_press_ms = -1;
            g_page = SS_SLEEP;
            /* Flush black frame then blank the panel */
            akira_display_clear(C_BLACK);
            akira_display_flush();
            akira_display_hal_set_blank(true);
            return;
        default:
            break;
        }
        redraw();
        return;
    }
    if (k & BIT(AKIRA_BTN_B))
    {
        /* Return to home launcher */
        g_active = false;
        home_screen_load();
        return;
    }
    if (ch)
        redraw();
}

static void handle_about(uint32_t k)
{
    if (k)
    {
        g_page = SS_MAIN;
        redraw();
    }
}

static void handle_bluetooth(uint32_t k)
{
    bool ch = false;
    if (k & BIT(AKIRA_BTN_UP))
    {
        g_bt_sel = (g_bt_sel - 1 + BT_ITEMS) % BT_ITEMS;
        g_bt_scroll = scroll_clamp(g_bt_sel, g_bt_scroll, BT_ITEMS, CONT_Y + 44);
        ch = true;
    }
    if (k & BIT(AKIRA_BTN_DOWN))
    {
        g_bt_sel = (g_bt_sel + 1) % BT_ITEMS;
        g_bt_scroll = scroll_clamp(g_bt_sel, g_bt_scroll, BT_ITEMS, CONT_Y + 44);
        ch = true;
    }
    if (k & BIT(AKIRA_BTN_A))
    {
        switch (g_bt_sel)
        {
        case 0: /* Adv ON */
#ifdef CONFIG_BT
            bt_manager_start_advertising();
#endif
            break;
        case 1: /* Adv OFF */
#ifdef CONFIG_BT
            bt_manager_stop_advertising();
#endif
            break;
        case 2: /* Unpair All */
#ifdef CONFIG_BT
            bt_manager_unpair_all();
#endif
            break;
#if defined(CONFIG_AKIRA_BT_COMPANION)
        case 3: /* BLE Mode — toggle HID <-> Companion (persist + reboot) */
        {
            const char *next =
                bt_manager_boot_mode_is_companion() ? "hid" : "companion";
            akira_settings_set(AKIRA_BT_MODE_KEY, next, 0);
            akira_display_clear(C_BLACK);
            akira_display_text(20, 60, "Switching BLE mode:", C_WHITE);
            akira_display_text(20, 78, next, C_GRAY);
            akira_display_text(20, 104, "Rebooting...", C_GRAY);
            akira_display_flush();
            k_sleep(K_MSEC(900));
            sys_reboot(SYS_REBOOT_COLD);
            break;
        }
        case 4: /* Back */
            g_page = SS_MAIN;
            break;
#else
        case 3: /* Back */
            g_page = SS_MAIN;
            break;
#endif
        }
        redraw();
        return;
    }
    if (k & BIT(AKIRA_BTN_B))
    {
        g_page = SS_MAIN;
        redraw();
        return;
    }
    if (ch)
        redraw();
}

static void handle_sleep(uint32_t k)
{
    if (!k)
        return;

    /* Only B wakes; ignore all other buttons */
    if (!(k & BIT(AKIRA_BTN_B)))
        return;

    int64_t now = k_uptime_get();

    if (g_sleep_first_press_ms < 0 ||
        (now - g_sleep_first_press_ms) > SLEEP_DPRESSW_MS)
    {
        /* First press — start the window */
        g_sleep_first_press_ms = now;
    }
    else
    {
        /* Second press within window — wake up */
        g_sleeping = false;
        g_sleep_first_press_ms = -1;
        akira_display_hal_set_blank(false);
        g_active = false;
        home_screen_load();
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void settings_screen_create(void)
{
    g_active = false;
    g_sleeping = false;
    g_sleep_first_press_ms = -1;
    g_page = SS_MAIN;
    g_main_sel = 0;
    g_main_scroll = 0;
    g_bt_sel = 0;
    g_bt_scroll = 0;
    LOG_INF("Settings screen created");
}

void settings_screen_load(void)
{
    g_active = true;
    g_page = SS_MAIN;
    g_main_sel = 0;
    g_main_scroll = 0;
    g_flush_next_key = true; /* discard stale B from returning sub-screen */
    redraw();
}

void settings_screen_handle_key(uint32_t just_pressed)
{
    if (!g_active)
        return;
    if (g_flush_next_key)
    {
        g_flush_next_key = false;
        /* Strip B/HOME from the very first event after returning from a
         * sub-screen so the button that caused the return is not replayed
         * here and does not immediately navigate to the home screen. */
        just_pressed &= ~(BIT(AKIRA_BTN_B) | BIT(AKIRA_BTN_HOME));
        if (!just_pressed)
            return;
    }
    switch (g_page)
    {
    case SS_MAIN:
        handle_main(just_pressed);
        break;
    case SS_BLUETOOTH:
        handle_bluetooth(just_pressed);
        break;
    case SS_ABOUT:
        handle_about(just_pressed);
        break;
    case SS_SLEEP:
        handle_sleep(just_pressed);
        break;
    default:
        break;
    }
}

void settings_screen_update(void)
{
    if (!g_active)
        return;
    /* Refresh status-showing pages on periodic tick */
    if (g_page == SS_BLUETOOTH)
    {
        redraw();
    }
}

bool settings_screen_is_active(void)
{
    return g_active;
}

bool settings_screen_is_sleeping(void)
{
    return g_sleeping;
}

void settings_screen_wake(void)
{
    if (!g_sleeping)
        return;
    g_sleeping = false;
    g_sleep_first_press_ms = -1;
    akira_display_hal_set_blank(false);
    g_active = false;
}
