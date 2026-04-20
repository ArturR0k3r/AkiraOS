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
 *
 * Navigation in WiFi text-input (append-only model):
 *   [UP/DN]  cycle char at end of field
 *   [A]      append current char (commits it)
 *   [X]      backspace (delete last char)
 *   [Y]      advance to next field / CONNECT button
 *   [B]      cancel — return to WiFi page
 */

#include "settings_screen.h"
#include "home_screen.h"
#include "settings/datetime_screen.h"
#include "settings/display_screen.h"
#include "settings/power_screen.h"
#include "settings/apps_screen.h"
#include "settings/ota_screen.h"
#include "settings/devmode_screen.h"

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <drivers/platform_hal.h>
#include <zephyr/version.h>

#ifdef CONFIG_AKIRA_SETTINGS
#include <settings/settings.h>
#endif

#ifdef CONFIG_BT
#include <connectivity/bluetooth/bt_manager.h>
#endif

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#endif

#if defined(CONFIG_AKIRA_HTTP_SERVER)
#include "connectivity/ota/web_server.h"
#endif

/* ------------------------------------------------------------------ */
/* Palette                                                            */
/* ------------------------------------------------------------------ */
#define C_BLACK      0x0000u
#define C_WHITE      0xFFFFu
#define C_GRAY       0x7BEFu
#define C_DKGRAY     0x39E7u
#define C_GLASS_HILIT 0x7BEFu
#define C_GLASS_BODY  0x0000u

/* ------------------------------------------------------------------ */
/* Geometry — Liquid Crystal layout (matches home_screen)            */
/* ------------------------------------------------------------------ */
#define SCR_W    320
#define SCR_H    240
#define SBAR_H   24        /* status bar height */
#define CONT_Y   26        /* content area top  */
#define CONT_H   188       /* 240-24-2-26 = content to ribbon */
#define RIB_Y    216
#define RIB_H    24

#define MENU_X   16
#define MENU_W   (SCR_W - 32)  /* 288 */
#define MENU_ITH 32            /* menu item height */

/* ------------------------------------------------------------------ */
/* Page state machine                                                  */
/* ------------------------------------------------------------------ */
typedef enum {
    SS_MAIN = 0,
    SS_WIFI,
    SS_WIFI_CONNECT,
    SS_WEBSERVER,
    SS_BLUETOOTH,
    SS_ABOUT,
    SS_SLEEP,
} ss_page_t;

static bool      g_active;
static ss_page_t g_page;

/* ---- Main menu --------------------------------------------------- */
#define MAIN_ITEMS 11
static const char *s_main_labels[MAIN_ITEMS] = {
    "WiFi", "Bluetooth", "Web Server",
    "Date & Time", "Display", "Power",
    "Apps", "OTA Update", "Developer",
    "About", "Sleep"
};
static int g_main_sel;

/* ---- WiFi menu (3 items) ----------------------------------------- */
#define WIFI_ITEMS 3
static const char *s_wifi_labels[WIFI_ITEMS] = {
    "Connect", "Disconnect", "Back"
};
static int g_wifi_sel;

/* ---- WiFi connect text input ------------------------------------- */
#define SSID_MAX     32
#define PSK_MAX      63
#define CONNECT_FIELD  2   /* third "field" is the CONNECT button */

static char    g_ssid[SSID_MAX + 1];
static char    g_psk[PSK_MAX  + 1];
static uint8_t g_ssid_cidx[SSID_MAX + 1]; /* per-position charset index */
static uint8_t g_psk_cidx[PSK_MAX  + 1];
static int     g_conn_field;   /* 0=SSID, 1=PSK, 2=CONNECT */

/* Printable charset for text picker */
static const char CHARSET[] =
    " abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    "!@#$%^&*()-_+=,./;:'\"";
#define CHARSET_LEN ((int)(sizeof(CHARSET) - 1))

/* ---- Web server menu (3 items) ----------------------------------- */
#define WS_ITEMS 3
static const char *s_ws_labels[WS_ITEMS] = {
    "Start", "Stop", "Back"
};
static int g_ws_sel;

/* ---- Bluetooth menu (4 items) ------------------------------------ */
#define BT_ITEMS 4
static const char *s_bt_labels[BT_ITEMS] = {
    "Adv ON", "Adv OFF", "Unpair All", "Back"
};
static int g_bt_sel;

/* ---- Scroll offsets (index of first visible item per menu) ------- */
static int g_main_scroll;
static int g_wifi_scroll;
static int g_ws_scroll;
static int g_bt_scroll;

/* ---- Sleep state ------------------------------------------------- */
static bool    g_sleeping;
/* Timestamp of first B press during sleep (ms); -1 = no pending press */
static int64_t g_sleep_first_press_ms;
#define SLEEP_DPRESSW_MS  500   /* double-press window in ms */
/* Set by settings_screen_load() to absorb the B press that came from a
 * sub-screen's blocking loop before settings re-enables its own handler. */
static bool    g_flush_next_key;

/* ------------------------------------------------------------------ */
/* Draw helpers                                                        */
/* ------------------------------------------------------------------ */
static void glass_rect(int x, int y, int w, int h, int r)
{
    akira_display_rounded_rect_fill(x, y, w, h, r, C_BLACK);
    akira_display_rounded_rect(x, y, w, h, r, C_WHITE);
    if (w > 4 && h > 4) {
        int ri = (r > 1) ? r - 1 : 0;
        akira_display_rounded_rect(x + 1, y + 1, w - 2, h - 2, ri, C_DKGRAY);
    }
    int ti = r + 2;
    if (w > ti * 2 && h > 6)
        akira_display_hline(x + ti, y + 3, w - ti * 2, C_GLASS_HILIT);
    if (w > 12 && h > 7) {
        int gw = w / 5; if (gw > 14) gw = 14;
        akira_display_rect(x + ti, y + 3, gw, 2, C_WHITE);
        if (gw > 4) akira_display_hline(x + ti, y + 5, gw / 2, C_GLASS_HILIT);
    }
    if (w > ti * 2 && h > 8)
        akira_display_hline(x + ti, y + h - 4, w - ti * 2, C_DKGRAY);
}

static void glass_rect_focus(int x, int y, int w, int h, int r)
{
    akira_display_rounded_rect_fill(x, y, w, h, r, C_BLACK);
    akira_display_rounded_rect(x, y, w, h, r, C_WHITE);
    if (w > 2 && h > 2) {
        int r1 = (r > 0) ? r - 1 : 0;
        akira_display_rounded_rect(x + 1, y + 1, w - 2, h - 2, r1, C_WHITE);
    }
    if (w > 6 && h > 6) {
        int r2 = (r > 1) ? r - 2 : 0;
        akira_display_rounded_rect(x + 2, y + 2, w - 4, h - 4, r2, C_DKGRAY);
    }
    int ti = r + 3;
    if (w > ti * 2 && h > 8)
        akira_display_hline(x + ti, y + 4, w - ti * 2, C_GLASS_HILIT);
    if (w > 14 && h > 9) {
        int gw = w / 5; if (gw > 16) gw = 16;
        akira_display_rect(x + ti, y + 4, gw, 2, C_WHITE);
        if (gw > 4) akira_display_hline(x + ti, y + 6, gw / 2, C_GLASS_HILIT);
    }
    if (w > ti * 2 && h > 10)
        akira_display_hline(x + ti, y + h - 5, w - ti * 2, C_DKGRAY);
    if (w > 14 && h > 11) {
        int gw = w / 6; if (gw > 12) gw = 12;
        akira_display_hline(x + w - ti - gw, y + h - 5, gw, C_WHITE);
        akira_display_hline(x + w - ti - gw, y + h - 4, gw / 2, C_GLASS_HILIT);
    }
}

static void glass_rect_dim(int x, int y, int w, int h, int r)
{
    akira_display_rounded_rect_fill(x, y, w, h, r, C_BLACK);
    akira_display_rounded_rect(x, y, w, h, r, C_DKGRAY);
    if (w > 4 && h > 4) {
        int ri = (r > 1) ? r - 1 : 0;
        akira_display_rounded_rect(x + 1, y + 1, w - 2, h - 2, ri, C_BLACK);
    }
    int ti = r + 2;
    if (w > ti * 2 && h > 6)
        akira_display_hline(x + ti, y + 3, w - ti * 2, C_DKGRAY);
    if (w > 12 && h > 7) {
        int gw = w / 5; if (gw > 14) gw = 14;
        akira_display_hline(x + ti, y + 3, gw, C_GRAY);
    }
    if (w > ti * 2 && h > 8)
        akira_display_hline(x + ti, y + h - 4, w - ti * 2, C_BLACK);
}

static void draw_centred(int x, int y, int w, const char *s,
                          uint16_t fg, uint16_t bg)
{
    int len = (int)strlen(s);
    int tw  = len * 8;
    int lx  = x + (tw < w ? (w - tw) / 2 : 0);
    akira_display_rect(x, y, w, 10, bg);
    akira_display_text(lx, y, s, fg);
}

static void draw_right(int rx, int y, const char *s, uint16_t col)
{
    akira_display_text(rx - (int)strlen(s) * 8, y, s, col);
}

static void draw_header(const char *title)
{
    akira_display_rect(0, 0, SCR_W, SBAR_H, C_BLACK);
    draw_centred(0, (SBAR_H - 10) / 2, SCR_W, title, C_WHITE, C_BLACK);
    akira_display_hline(0, SBAR_H,     SCR_W, C_WHITE);
    akira_display_hline(0, SBAR_H + 1, SCR_W, C_WHITE);
}

static void draw_ribbon(const char *left, const char *right)
{
    akira_display_hline(0, RIB_Y - 1, SCR_W, C_WHITE);
    akira_display_hline(0, RIB_Y - 2, SCR_W, C_WHITE);
    akira_display_rect(0, RIB_Y, SCR_W, RIB_H, C_BLACK);
    if (left  && *left)  akira_display_text(8, RIB_Y + 7, left,  C_WHITE);
    if (right && *right) draw_right(SCR_W - 8, RIB_Y + 7, right, C_WHITE);
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
    if (sel < scroll)        return sel;
    if (sel >= scroll + vis) return sel - vis + 1;
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

    for (int i = scroll; i < count && i < scroll + vis; i++) {
        int row = i - scroll;
        int iy  = top_y + row * MENU_ITH;
        bool hi  = (i == sel);
        int bx = MENU_X, by = iy + 2, bw = MENU_W, bh = MENU_ITH - 4;

        if (hi) {
            glass_rect_focus(bx, by, bw, bh, 5);
        } else {
            glass_rect_dim(bx, by, bw, bh, 5);
        }
        int ty = by + (bh - 10) / 2;
        uint16_t fg = hi ? C_WHITE : C_DKGRAY;
        draw_centred(bx + 4, ty, bw - 28, labels[i], fg, C_GLASS_BODY);
        akira_display_text(bx + bw - 18, ty, ">", fg);
    }

    /* Scroll indicators in the right margin (x > MENU_X+MENU_W) */
    if (scroll > 0) {
        akira_display_text(SCR_W - 10, top_y + 2, "^", C_GRAY);
    }
    if (scroll + vis < count) {
        int bot_y = top_y + vis * MENU_ITH + 2;
        if (bot_y < RIB_Y - 10) {
            akira_display_text(SCR_W - 10, bot_y, "v", C_GRAY);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Shared draw helpers — non-static, used by settings sub-screens     */
/* ------------------------------------------------------------------ */
void ss_glass_rect_focus(int x, int y, int w, int h, int r) { glass_rect_focus(x, y, w, h, r); }
void ss_glass_rect_dim  (int x, int y, int w, int h, int r) { glass_rect_dim(x, y, w, h, r);   }
void ss_draw_centred(int x, int y, int w, const char *s, uint16_t fg, uint16_t bg) { draw_centred(x, y, w, s, fg, bg); }
void ss_draw_header(const char *title)                                              { draw_header(title);               }
void ss_draw_ribbon(const char *left, const char *right)                            { draw_ribbon(left, right);         }
int  ss_menu_vis_count(int top_y)                                                   { return menu_vis_count(top_y);     }
int  ss_scroll_clamp(int sel, int scroll, int count, int top_y)                     { return scroll_clamp(sel, scroll, count, top_y); }
void ss_draw_menu_at(const char **labels, int count, int sel, int top_y, int scroll) { draw_menu_at(labels, count, sel, top_y, scroll); }

/* ------------------------------------------------------------------ */
/* WiFi helpers                                                        */
/* ------------------------------------------------------------------ */
static bool wifi_get_status(char *ssid_out, size_t ssid_sz,
                             char *state_out, size_t state_sz)
{
    strncpy(ssid_out,  "---",          ssid_sz  - 1);
    strncpy(state_out, "DISCONNECTED", state_sz - 1);
    ssid_out[ssid_sz - 1]   = '\0';
    state_out[state_sz - 1] = '\0';

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        return false;
    }
    struct wifi_iface_status st = {0};
    if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &st, sizeof(st)) == 0 &&
        st.state >= WIFI_STATE_ASSOCIATED) {
        int slen = (int)st.ssid_len;
        if (slen > (int)ssid_sz - 1) slen = (int)ssid_sz - 1;
        memcpy(ssid_out, st.ssid, slen);
        ssid_out[slen] = '\0';
        strncpy(state_out, "CONNECTED", state_sz - 1);
        return true;
    }
#endif
    return false;
}

static void wifi_get_ip(char *buf, size_t len)
{
    strncpy(buf, "---", len - 1);
    buf[len - 1] = '\0';

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
    struct net_if *iface = net_if_get_default();
    if (!iface) return;
    struct in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (addr) {
        net_addr_ntop(AF_INET, addr, buf, (socklen_t)len);
    }
#endif
}

static void do_wifi_connect(void)
{
#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
    /* If user left SSID empty, try loading saved credentials from NVS */
    if (g_ssid[0] == '\0') {
#ifdef CONFIG_AKIRA_SETTINGS
        akira_settings_get(AKIRA_SETTINGS_WIFI_SSID_KEY, g_ssid, sizeof(g_ssid));
        akira_settings_get(AKIRA_SETTINGS_WIFI_PSK_KEY,  g_psk,  sizeof(g_psk));
        LOG_INF("WiFi: loaded saved credentials for '%s'", g_ssid);
#endif
    }
    if (g_ssid[0] == '\0') {
        LOG_ERR("WiFi: no SSID provided and no saved credentials");
        return;
    }
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        LOG_ERR("No network interface for WiFi connect");
        return;
    }
    struct wifi_connect_req_params p = {
        .ssid        = (const uint8_t *)g_ssid,
        .ssid_length = (uint8_t)strlen(g_ssid),
        .psk         = (const uint8_t *)g_psk,
        .psk_length  = (uint8_t)strlen(g_psk),
        .security    = strlen(g_psk) ? WIFI_SECURITY_TYPE_PSK
                                     : WIFI_SECURITY_TYPE_NONE,
        .channel     = WIFI_CHANNEL_ANY,
        .mfp         = WIFI_MFP_OPTIONAL,
    };
    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p));
    if (ret < 0) {
        LOG_ERR("WiFi connect failed: %d", ret);
    } else {
        LOG_INF("WiFi connecting to '%s'", g_ssid);
#ifdef CONFIG_AKIRA_SETTINGS
        /* Persist credentials for next time */
        akira_settings_set(AKIRA_SETTINGS_WIFI_SSID_KEY, g_ssid, 0);
        akira_settings_set(AKIRA_SETTINGS_WIFI_PSK_KEY,  g_psk,  0);
#endif
    }
#else
    LOG_WRN("WiFi support not compiled in");
#endif
}

static void do_wifi_disconnect(void)
{
#if defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
    struct net_if *iface = net_if_get_default();
    if (!iface) return;
    int ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
    if (ret < 0) {
        LOG_ERR("WiFi disconnect failed: %d", ret);
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Page renderers                                                      */
/* ------------------------------------------------------------------ */
static void draw_main(void)
{
    draw_header("SETTINGS");
    akira_display_rect(0, CONT_Y, SCR_W, CONT_H, C_BLACK);
    draw_menu_at(s_main_labels, MAIN_ITEMS, g_main_sel, CONT_Y, g_main_scroll);
    draw_ribbon("[A] SELECT", "[B] HOME");
}

static void draw_wifi(void)
{
    draw_header("WiFi");
    akira_display_rect(0, CONT_Y, SCR_W, CONT_H, C_BLACK);

    char ssid[33] = {0}, state[16] = {0}, ip[20] = {0};
    wifi_get_status(ssid, sizeof(ssid), state, sizeof(state));
    wifi_get_ip(ip, sizeof(ip));

    char l1[48], l2[48];
    snprintf(l1, sizeof(l1), "Status: %s", state);
    snprintf(l2, sizeof(l2), "SSID: %-14s IP: %s", ssid, ip);

    akira_display_rounded_rect_fill(4, CONT_Y + 4, SCR_W - 8, 34, 3, C_BLACK);
    akira_display_rounded_rect(4, CONT_Y + 4, SCR_W - 8, 34, 3, C_DKGRAY);
    akira_display_text(10, CONT_Y + 10, l1, C_WHITE);
    akira_display_text(10, CONT_Y + 24, l2, C_WHITE);

    draw_menu_at(s_wifi_labels, WIFI_ITEMS, g_wifi_sel, CONT_Y + 44, g_wifi_scroll);
    draw_ribbon("[A] SELECT", "[B] BACK");
}

static void draw_wifi_connect(void)
{
    draw_header("WiFi Connect");
    akira_display_rect(0, CONT_Y, SCR_W, CONT_H, C_BLACK);

    const int FX = 8, FW = SCR_W - 16, FH = 28;

    /* Helper: draw one text field */
    for (int f = 0; f < 2; f++) {
        bool active  = (g_conn_field == f);
        uint16_t fbg = active ? C_WHITE : C_BLACK;
        uint16_t ffg = active ? C_BLACK : C_WHITE;

        const char *label = (f == 0) ? "SSID:" : "PSK:";
        char       *buf   = (f == 0) ? g_ssid  : g_psk;
        uint8_t    *cidx  = (f == 0) ? g_ssid_cidx : g_psk_cidx;

        int fy = CONT_Y + 6 + f * (FH + 20);

        akira_display_text(FX, fy, label, C_WHITE);
        akira_display_rect(FX, fy + 12, FW, FH, fbg);
        akira_display_rect_outline(FX, fy + 12, FW, FH, active ? C_BLACK : C_GRAY);

        int flen = (int)strlen(buf);

        /* Build display string: committed + pending char at end */
        char disp[SSID_MAX + 2];
        if (f == 0) {
            memcpy(disp, buf, flen);
            if (active) {
                disp[flen] = CHARSET[cidx[flen]];
                disp[flen + 1] = '\0';
            } else {
                disp[flen] = '\0';
            }
        } else {
            /* Hide PSK chars with '*'; show pending at end if active */
            for (int i = 0; i < flen; i++) disp[i] = '*';
            if (active) {
                disp[flen] = CHARSET[cidx[flen]];
                disp[flen + 1] = '\0';
            } else {
                disp[flen] = '\0';
            }
        }

        int text_x = FX + 4;
        int text_y = fy + 12 + (FH - 10) / 2;
        akira_display_text(text_x, text_y, disp, ffg);

        /* Underline cursor at end of string */
        if (active) {
            int cx = text_x + flen * 8;
            akira_display_hline(cx, fy + 12 + FH - 3, 8, ffg);
        }
    }

    /* CONNECT button */
    int btn_y = CONT_Y + 6 + 2 * (FH + 20) + 8;
    bool btn_hi = (g_conn_field == CONNECT_FIELD);
    if (btn_hi) {
        glass_rect_focus(MENU_X, btn_y, MENU_W, 34, 5);
    } else {
        glass_rect(MENU_X, btn_y, MENU_W, 34, 5);
    }
    uint16_t bfg = btn_hi ? C_WHITE : C_GRAY;
    draw_centred(MENU_X + 4, btn_y + 12, MENU_W - 8, "CONNECT", bfg, C_GLASS_BODY);

    draw_ribbon("[UP/DN] char  [A] add  [X] del  [Y] next", "[B] BACK");
}

static void draw_webserver(void)
{
    draw_header("Web Server");
    akira_display_rect(0, CONT_Y, SCR_W, CONT_H, C_BLACK);

    char l1[48] = "Status: STOPPED";
    char l2[64] = "URL: http://---:" STRINGIFY(HTTP_PORT);

#if defined(CONFIG_AKIRA_HTTP_SERVER)
    if (web_server_is_running()) {
        char ip[20] = "---";
        wifi_get_ip(ip, sizeof(ip));
        snprintf(l1, sizeof(l1), "Status: RUNNING");
        snprintf(l2, sizeof(l2), "URL: http://%s:%d", ip, HTTP_PORT);
    }
#endif

    akira_display_rounded_rect_fill(4, CONT_Y + 4, SCR_W - 8, 34, 3, C_BLACK);
    akira_display_rounded_rect(4, CONT_Y + 4, SCR_W - 8, 34, 3, C_DKGRAY);
    akira_display_text(10, CONT_Y + 10, l1, C_WHITE);
    akira_display_text(10, CONT_Y + 24, l2, C_WHITE);

    draw_menu_at(s_ws_labels, WS_ITEMS, g_ws_sel, CONT_Y + 44, g_ws_scroll);
    draw_ribbon("[A] SELECT", "[B] BACK");
}

static void draw_about(void)
{
    draw_header("About AkiraOS");
    akira_display_rect(0, CONT_Y, SCR_W, CONT_H, C_BLACK);

    char lines[5][48];
    snprintf(lines[0], 48, "AkiraOS  v%s", CONFIG_AKIRA_OS_VERSION);
    snprintf(lines[1], 48, "Board:   %s",  CONFIG_BOARD);
    snprintf(lines[2], 48, "Built:   %s",  __DATE__);
    snprintf(lines[4], 48, "WASM micro runtime embedded");

    int sy = CONT_Y + (CONT_H - 5 * 22) / 2;
    for (int i = 0; i < 5; i++) {
        akira_display_text(16, sy + i * 22, lines[i], C_WHITE);
    }
    draw_ribbon("", "[B] BACK");
}

static void draw_bluetooth(void)
{
    draw_header("Bluetooth");
    akira_display_rect(0, CONT_Y, SCR_W, CONT_H, C_BLACK);

    char state_str[20] = "OFF";
    char addr_str[20]  = "---";

#ifdef CONFIG_BT
    bt_state_t bts = bt_manager_get_state();
    if (bts == BT_STATE_ADVERTISING)      strncpy(state_str, "ADVERTISING",  sizeof(state_str) - 1);
    else if (bts == BT_STATE_CONNECTED)   strncpy(state_str, "CONNECTED",    sizeof(state_str) - 1);
    else if (bts == BT_STATE_READY)       strncpy(state_str, "READY",        sizeof(state_str) - 1);
    else if (bts == BT_STATE_PAIRING)     strncpy(state_str, "PAIRING",      sizeof(state_str) - 1);
    else if (bts == BT_STATE_INITIALIZING) strncpy(state_str, "INIT",        sizeof(state_str) - 1);
    bt_manager_get_address(addr_str, sizeof(addr_str));
#endif

    char l1[48], l2[48];
    snprintf(l1, sizeof(l1), "State: %s",   state_str);
    snprintf(l2, sizeof(l2), "Addr:  %s",   addr_str);

    akira_display_rounded_rect_fill(4, CONT_Y + 4, SCR_W - 8, 34, 3, C_BLACK);
    akira_display_rounded_rect(4, CONT_Y + 4, SCR_W - 8, 34, 3, C_DKGRAY);
    akira_display_text(10, CONT_Y + 10, l1, C_WHITE);
    akira_display_text(10, CONT_Y + 24, l2, C_WHITE);

    draw_menu_at(s_bt_labels, BT_ITEMS, g_bt_sel, CONT_Y + 44, g_bt_scroll);
    draw_ribbon("[A] SELECT", "[B] BACK");
}


static void redraw(void)
{
    akira_display_clear(C_BLACK);
    switch (g_page) {
    case SS_MAIN:         draw_main();          break;
    case SS_WIFI:          draw_wifi();           break;
    case SS_WIFI_CONNECT:  draw_wifi_connect();   break;
    case SS_WEBSERVER:     draw_webserver();      break;
    case SS_BLUETOOTH:     draw_bluetooth();      break;
    case SS_ABOUT:         draw_about();          break;
    case SS_SLEEP:         /* nothing — display is blanked */ break;
    default:                                      break;
    }
    akira_display_flush();
}

/* ------------------------------------------------------------------ */
/* Key handlers                                                        */
/* ------------------------------------------------------------------ */
static void handle_main(uint32_t k)
{
    bool ch = false;
    if (k & BIT(AKIRA_BTN_UP)) {
        g_main_sel = (g_main_sel - 1 + MAIN_ITEMS) % MAIN_ITEMS;
        g_main_scroll = scroll_clamp(g_main_sel, g_main_scroll, MAIN_ITEMS, CONT_Y);
        ch = true;
    }
    if (k & BIT(AKIRA_BTN_DOWN)) {
        g_main_sel = (g_main_sel + 1) % MAIN_ITEMS;
        g_main_scroll = scroll_clamp(g_main_sel, g_main_scroll, MAIN_ITEMS, CONT_Y);
        ch = true;
    }
    if (k & BIT(AKIRA_BTN_A)) {
        switch (g_main_sel) {
        case 0:
            g_wifi_sel    = 0;
            g_wifi_scroll = 0;
            g_page = SS_WIFI;
            break;
        case 1:
            g_bt_sel    = 0;
            g_bt_scroll = 0;
            g_page = SS_BLUETOOTH;
            break;
        case 2:
            g_ws_sel    = 0;
            g_ws_scroll = 0;
            g_page = SS_WEBSERVER;
            break;
        case 3:
            /* Date & Time — hands off to datetime_screen (blocking loop) */
            g_active = false;
            datetime_screen_load();
            return;
        case 4:
            g_active = false;
            display_screen_load();
            return;
        case 5:
            g_active = false;
            power_screen_load();
            return;
        case 6:
            g_active = false;
            apps_screen_load();
            return;
        case 7:
            g_active = false;
            ota_screen_load();
            return;
        case 8:
            g_active = false;
            devmode_screen_load();
            return;
        case 9:
            g_page = SS_ABOUT;
            break;
        case 10: /* Sleep */
            g_sleeping = true;
            g_sleep_first_press_ms = -1;
            g_page = SS_SLEEP;
            /* Flush black frame then blank the panel */
            akira_display_clear(C_BLACK);
            akira_display_flush();
            akira_display_hal_set_blank(true);
            return;
        }
        redraw();
        return;
    }
    if (k & BIT(AKIRA_BTN_B)) {
        /* Return to home launcher */
        g_active = false;
        home_screen_load();
        return;
    }
    if (ch) redraw();
}

static void handle_wifi(uint32_t k)
{
    bool ch = false;
    if (k & BIT(AKIRA_BTN_UP)) {
        g_wifi_sel = (g_wifi_sel - 1 + WIFI_ITEMS) % WIFI_ITEMS;
        g_wifi_scroll = scroll_clamp(g_wifi_sel, g_wifi_scroll, WIFI_ITEMS, CONT_Y + 44);
        ch = true;
    }
    if (k & BIT(AKIRA_BTN_DOWN)) {
        g_wifi_sel = (g_wifi_sel + 1) % WIFI_ITEMS;
        g_wifi_scroll = scroll_clamp(g_wifi_sel, g_wifi_scroll, WIFI_ITEMS, CONT_Y + 44);
        ch = true;
    }
    if (k & BIT(AKIRA_BTN_A)) {
        switch (g_wifi_sel) {
        case 0: /* Connect → text input screen */
            memset(g_ssid, 0, sizeof(g_ssid));
            memset(g_psk,  0, sizeof(g_psk));
            memset(g_ssid_cidx, 0, sizeof(g_ssid_cidx));
            memset(g_psk_cidx,  0, sizeof(g_psk_cidx));
#ifdef CONFIG_AKIRA_SETTINGS
            /* Pre-populate with saved credentials as default */
            akira_settings_get(AKIRA_SETTINGS_WIFI_SSID_KEY, g_ssid, sizeof(g_ssid));
            akira_settings_get(AKIRA_SETTINGS_WIFI_PSK_KEY,  g_psk,  sizeof(g_psk));
#endif
            g_conn_field = 0;
            g_page = SS_WIFI_CONNECT;
            break;
        case 1: /* Disconnect */
            do_wifi_disconnect();
            break;
        case 2: /* Back */
            g_page = SS_MAIN;
            break;
        }
        redraw();
        return;
    }
    if (k & BIT(AKIRA_BTN_B)) {
        g_page = SS_MAIN;
        redraw();
        return;
    }
    if (ch) redraw();
}

static void handle_wifi_connect(uint32_t k)
{
    if (k & BIT(AKIRA_BTN_B)) {
        g_page = SS_WIFI;
        redraw();
        return;
    }

    if (g_conn_field == CONNECT_FIELD) {
        /* On CONNECT button */
        if (k & BIT(AKIRA_BTN_A)) {
            do_wifi_connect();
            g_page = SS_WIFI;
            redraw();
            return;
        }
        if (k & BIT(AKIRA_BTN_UP)) {
            g_conn_field = 1;   /* go back to PSK field */
            redraw();
            return;
        }
    } else {
        /* On a text field (0=SSID, 1=PSK) */
        char    *buf  = (g_conn_field == 0) ? g_ssid : g_psk;
        int      mlen = (g_conn_field == 0) ? SSID_MAX : PSK_MAX;
        uint8_t *cidx = (g_conn_field == 0) ? g_ssid_cidx : g_psk_cidx;
        int      flen = (int)strlen(buf);
        bool     ch   = false;

        if (k & BIT(AKIRA_BTN_UP)) {
            /* Cycle char forward */
            cidx[flen] = (uint8_t)((cidx[flen] + 1) % CHARSET_LEN);
            ch = true;
        }
        if (k & BIT(AKIRA_BTN_DOWN)) {
            /* Cycle char backward */
            cidx[flen] = (uint8_t)((cidx[flen] + CHARSET_LEN - 1) % CHARSET_LEN);
            ch = true;
        }
        if (k & BIT(AKIRA_BTN_A)) {
            /* Append current char */
            if (flen < mlen) {
                buf[flen]     = CHARSET[cidx[flen]];
                buf[flen + 1] = '\0';
                /* Next position starts at space */
                cidx[flen + 1] = 0;
                ch = true;
            }
        }
        if (k & BIT(AKIRA_BTN_X)) {
            /* Backspace */
            if (flen > 0) {
                buf[flen - 1] = '\0';
                ch = true;
            }
        }
        if (k & BIT(AKIRA_BTN_Y)) {
            /* Advance to next field */
            g_conn_field++;
            ch = true;
        }
        if (ch) redraw();
    }
}

static void handle_webserver(uint32_t k)
{
    bool ch = false;
    if (k & BIT(AKIRA_BTN_UP)) {
        g_ws_sel = (g_ws_sel - 1 + WS_ITEMS) % WS_ITEMS;
        g_ws_scroll = scroll_clamp(g_ws_sel, g_ws_scroll, WS_ITEMS, CONT_Y + 44);
        ch = true;
    }
    if (k & BIT(AKIRA_BTN_DOWN)) {
        g_ws_sel = (g_ws_sel + 1) % WS_ITEMS;
        g_ws_scroll = scroll_clamp(g_ws_sel, g_ws_scroll, WS_ITEMS, CONT_Y + 44);
        ch = true;
    }
    if (k & BIT(AKIRA_BTN_A)) {
        switch (g_ws_sel) {
        case 0: /* Start — notify server thread of current IP (thread started at boot) */
#if defined(CONFIG_AKIRA_HTTP_SERVER) && defined(CONFIG_WIFI) && defined(CONFIG_NET_MGMT)
        {
            char ws_ip[NET_IPV4_ADDR_LEN] = "0.0.0.0";
            struct net_if *ws_iface = net_if_get_default();
            if (ws_iface) {
                struct in_addr *ws_addr =
                    net_if_ipv4_get_global_addr(ws_iface, NET_ADDR_PREFERRED);
                if (ws_addr) {
                    net_addr_ntop(AF_INET, ws_addr, ws_ip, sizeof(ws_ip));
                }
            }
            web_server_notify_network_status(true, ws_ip);
        }
#elif defined(CONFIG_AKIRA_HTTP_SERVER)
            web_server_notify_network_status(true, "0.0.0.0");
#else
            LOG_WRN("HTTP server not compiled in");
#endif
            break;
        case 1: /* Stop */
#if defined(CONFIG_AKIRA_HTTP_SERVER)
            web_server_stop();
#else
            LOG_WRN("HTTP server not compiled in");
#endif
            break;
        case 2: /* Back */
            g_page = SS_MAIN;
            break;
        }
        redraw();
        return;
    }
    if (k & BIT(AKIRA_BTN_B)) {
        g_page = SS_MAIN;
        redraw();
        return;
    }
    if (ch) redraw();
}

static void handle_about(uint32_t k)
{
    if (k) {
        g_page = SS_MAIN;
        redraw();
    }
}

static void handle_bluetooth(uint32_t k)
{
    bool ch = false;
    if (k & BIT(AKIRA_BTN_UP)) {
        g_bt_sel = (g_bt_sel - 1 + BT_ITEMS) % BT_ITEMS;
        g_bt_scroll = scroll_clamp(g_bt_sel, g_bt_scroll, BT_ITEMS, CONT_Y + 44);
        ch = true;
    }
    if (k & BIT(AKIRA_BTN_DOWN)) {
        g_bt_sel = (g_bt_sel + 1) % BT_ITEMS;
        g_bt_scroll = scroll_clamp(g_bt_sel, g_bt_scroll, BT_ITEMS, CONT_Y + 44);
        ch = true;
    }
    if (k & BIT(AKIRA_BTN_A)) {
        switch (g_bt_sel) {
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
        case 3: /* Back */
            g_page = SS_MAIN;
            break;
        }
        redraw();
        return;
    }
    if (k & BIT(AKIRA_BTN_B)) {
        g_page = SS_MAIN;
        redraw();
        return;
    }
    if (ch) redraw();
}


static void handle_sleep(uint32_t k)
{
    if (!k) return;

    /* Only B wakes; ignore all other buttons */
    if (!(k & BIT(AKIRA_BTN_B))) return;

    int64_t now = k_uptime_get();

    if (g_sleep_first_press_ms < 0 ||
        (now - g_sleep_first_press_ms) > SLEEP_DPRESSW_MS) {
        /* First press — start the window */
        g_sleep_first_press_ms = now;
    } else {
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
    g_active     = false;
    g_sleeping   = false;
    g_sleep_first_press_ms = -1;
    g_page       = SS_MAIN;
    g_main_sel   = 0; g_main_scroll = 0;
    g_wifi_sel   = 0; g_wifi_scroll = 0;
    g_ws_sel     = 0; g_ws_scroll   = 0;
    g_bt_sel     = 0; g_bt_scroll   = 0;
    g_conn_field = 0;
    memset(g_ssid,       0, sizeof(g_ssid));
    memset(g_psk,        0, sizeof(g_psk));
    memset(g_ssid_cidx,  0, sizeof(g_ssid_cidx));
    memset(g_psk_cidx,   0, sizeof(g_psk_cidx));
    LOG_INF("Settings screen created");
}

void settings_screen_load(void)
{
    g_active         = true;
    g_page           = SS_MAIN;
    g_main_sel       = 0;
    g_main_scroll    = 0;
    g_flush_next_key = true;   /* discard stale B from returning sub-screen */
    redraw();
}

void settings_screen_handle_key(uint32_t just_pressed)
{
    if (!g_active) return;
    if (g_flush_next_key) {
        g_flush_next_key = false;
        /* Strip B/HOME from the very first event after returning from a
         * sub-screen so the button that caused the return is not replayed
         * here and does not immediately navigate to the home screen. */
        just_pressed &= ~(BIT(AKIRA_BTN_B) | BIT(AKIRA_BTN_HOME));
        if (!just_pressed) return;
    }
    switch (g_page) {
    case SS_MAIN:         handle_main(just_pressed);          break;
    case SS_WIFI:          handle_wifi(just_pressed);           break;
    case SS_WIFI_CONNECT:  handle_wifi_connect(just_pressed);   break;
    case SS_WEBSERVER:     handle_webserver(just_pressed);      break;
    case SS_BLUETOOTH:     handle_bluetooth(just_pressed);      break;
    case SS_ABOUT:         handle_about(just_pressed);          break;
    case SS_SLEEP:         handle_sleep(just_pressed);          break;
    default:                                                     break;
    }
}

void settings_screen_update(void)
{
    if (!g_active) return;
    /* Refresh status-showing pages on periodic tick */
    if (g_page == SS_WIFI || g_page == SS_WEBSERVER || g_page == SS_BLUETOOTH) {
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
