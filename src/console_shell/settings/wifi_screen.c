/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#define LOG_MODULE_NAME akira_wifi_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_wifi_screen, CONFIG_AKIRA_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

#include "shell_theme.h"
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include "wifi_screen.h"

#include <connectivity/wifi/wifi_manager.h>

#define WIFI_MAX_NETWORKS 16
static wifi_mgr_scan_result_t g_networks[WIFI_MAX_NETWORKS] __attribute__((section(".ext_ram.bss")));
static int g_network_count;

static struct k_sem g_scan_done;
static struct k_sem g_connect_done;
static struct k_sem g_disconnect_done;
static int  g_connect_result; /* 1 = connected, 0 = failed, set before g_connect_done is given */
static bool g_wifi_mgr_cb_reg;

static void on_wifi_mgr_event(wifi_mgr_event_t evt, void *user_data)
{
    ARG_UNUSED(user_data);
    switch (evt) {
    case WIFI_MGR_EVT_SCAN_DONE:
        k_sem_give(&g_scan_done);
        break;
    case WIFI_MGR_EVT_CONNECTED:
        g_connect_result = 1;
        k_sem_give(&g_connect_done);
        break;
    case WIFI_MGR_EVT_CONNECT_FAILED:
        g_connect_result = 0;
        k_sem_give(&g_connect_done);
        break;
    case WIFI_MGR_EVT_DISCONNECTED:
        k_sem_give(&g_disconnect_done);
        break;
    default:
        break;
    }
}

static void ensure_wifi_mgr_cb(void)
{
    if (!g_wifi_mgr_cb_reg) {
        k_sem_init(&g_scan_done, 0, 1);
        k_sem_init(&g_connect_done, 0, 1);
        k_sem_init(&g_disconnect_done, 0, 1);
        wifi_manager_register_cb(on_wifi_mgr_event, NULL);
        g_wifi_mgr_cb_reg = true;
    }
}



static void lc_centred(int x, int y, int w, const char *s, uint16_t fg, uint16_t bg) {
    int tw = (int)(strlen(s)*8); int lx = x+(tw<w?(w-tw)/2:0);
    akira_display_rect(x,y,w,10,bg); akira_display_text(lx,y,s,fg);
}
static void lc_header(const char *title) {
    akira_display_rect(0,0,SCR_W,SBAR_H,C_BLACK);
    lc_centred(0,(SBAR_H-10)/2,SCR_W,title,C_WHITE,C_BLACK);
    akira_display_hline(0,SBAR_H,SCR_W,C_WHITE); akira_display_hline(0,SBAR_H+1,SCR_W,C_WHITE);
}
static void lc_footer(const char *txt) {
    akira_display_hline(0,FOOT_Y-1,SCR_W,C_WHITE); akira_display_hline(0,FOOT_Y-2,SCR_W,C_WHITE);
    akira_display_rect(0,FOOT_Y,SCR_W,FOOT_H,C_BLACK); akira_display_text(8,FOOT_Y+7,txt,C_WHITE);
}
static void lc_item(int idx, int sel, const char *lbl, const char *rv) {
    int iy=LIST_Y+idx*ITEM_H; bool hi=(idx==sel);
    uint16_t ibg=hi?C_WHITE:C_BLACK, ifg=hi?C_BLACK:C_WHITE;
    akira_display_rounded_rect_fill(ITEM_X,iy+3,ITEM_W,ITEM_H-6,5,ibg);
    akira_display_rounded_rect(ITEM_X,iy+3,ITEM_W,ITEM_H-6,5,hi?C_BLACK:C_DKGRAY);
    int ty=iy+3+(ITEM_H-6-10)/2;
    akira_display_text(ITEM_X+10,ty,lbl,ifg);
    if (rv&&rv[0]){int tw=(int)strlen(rv)*8; akira_display_text(ITEM_X+ITEM_W-tw-10,ty,rv,ifg);}
}
static void lc_bar(int x, int y, int w, int h, int pct) {
    akira_display_rect_outline(x-1,y-1,w+2,h+2,C_WHITE);
    int fw = pct*w/100;
    akira_display_rect(x,y,w,h,C_DKGRAY);
    if (fw>0) akira_display_rect(x,y,fw,h,C_WHITE);
}

#define PASS_MAX 64

static int  g_sel, g_scroll;

/* ---- On-screen QWERTY keyboard for password entry ---- */
static const char *const KB_ROWS_LOWER[4] = {"1234567890","qwertyuiop","asdfghjkl","zxcvbnm"};
static const char *const KB_ROWS_UPPER[4] = {"!@#$%^&*()","QWERTYUIOP","ASDFGHJKL","ZXCVBNM"};
static const int KB_ROW_LEN[5] = {10,10,9,7,1}; /* row 4 = space bar, 1 "column" */

#define KB_MARGIN_X 8
#define KB_CELL_W   38
#define KB_ROW_H    30
#define KB_Y0       (LIST_Y + 22)

static bool g_kb_shift;
static int  g_kb_row, g_kb_col;
static char g_kb_buf[PASS_MAX];
static int  g_kb_len;

static int kb_row_x0(int row) {
    switch (row) {
    case 2: return KB_MARGIN_X + KB_CELL_W / 2;
    case 3: return KB_MARGIN_X + KB_CELL_W + KB_CELL_W / 2;
    case 4: return KB_MARGIN_X + KB_CELL_W * 2;
    default: return KB_MARGIN_X;
    }
}

static void kb_move(int dr, int dc) {
    if (dr != 0) {
        g_kb_row = (g_kb_row + dr + 5) % 5;
        if (g_kb_col >= KB_ROW_LEN[g_kb_row]) {
            g_kb_col = KB_ROW_LEN[g_kb_row] - 1;
        }
    }
    if (dc != 0) {
        int len = KB_ROW_LEN[g_kb_row];
        g_kb_col = (g_kb_col + dc + len) % len;
    }
}

static void draw_keyboard(const char *ssid) {
    akira_display_clear(C_BLACK);
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "Password: %s", ssid);
    lc_header(hdr);

    char disp[PASS_MAX + 1];
    memcpy(disp, g_kb_buf, g_kb_len);
    disp[g_kb_len] = 0;
    akira_display_text(KB_MARGIN_X, LIST_Y + 2, disp, C_WHITE);

    for (int row = 0; row < 4; row++) {
        const char *chars = g_kb_shift ? KB_ROWS_UPPER[row] : KB_ROWS_LOWER[row];
        int x0 = kb_row_x0(row);
        int y = KB_Y0 + row * KB_ROW_H;
        for (int col = 0; col < KB_ROW_LEN[row]; col++) {
            bool hi = (row == g_kb_row && col == g_kb_col);
            int x = x0 + col * KB_CELL_W;
            uint16_t bg = hi ? C_WHITE : C_BLACK;
            uint16_t fg = hi ? C_BLACK : C_WHITE;
            akira_display_rounded_rect_fill(x, y, KB_CELL_W - 2, KB_ROW_H - 4, 3, bg);
            akira_display_rounded_rect(x, y, KB_CELL_W - 2, KB_ROW_H - 4, 3, hi ? C_BLACK : C_DKGRAY);
            char cs[2] = {chars[col], 0};
            akira_display_text(x + KB_CELL_W / 2 - 4, y + (KB_ROW_H - 4 - 10) / 2, cs, fg);
        }
    }

    {
        int row = 4;
        bool hi = (g_kb_row == row);
        int x = kb_row_x0(row);
        int w = KB_CELL_W * 6 - 2;
        int y = KB_Y0 + row * KB_ROW_H;
        uint16_t bg = hi ? C_WHITE : C_BLACK;
        uint16_t fg = hi ? C_BLACK : C_WHITE;
        akira_display_rounded_rect_fill(x, y, w, KB_ROW_H - 4, 3, bg);
        akira_display_rounded_rect(x, y, w, KB_ROW_H - 4, 3, hi ? C_BLACK : C_DKGRAY);
        akira_display_text(x + w / 2 - 20, y + (KB_ROW_H - 4 - 10) / 2, "SPACE", fg);
    }

    lc_footer("A:type  B:del  Y:shift  X:connect");
    akira_display_flush();
}

static bool enter_password(const char *ssid, char *out, int maxlen, const char *initial) {
    g_kb_shift = false;
    g_kb_row = 1; /* start on the qwerty row */
    g_kb_col = 0;
    g_kb_len = 0;
    memset(g_kb_buf, 0, sizeof(g_kb_buf));
    if (initial && initial[0]) {
        g_kb_len = (int)strlen(initial);
        if (g_kb_len > maxlen - 1) g_kb_len = maxlen - 1;
        memcpy(g_kb_buf, initial, g_kb_len);
    }
    draw_keyboard(ssid);

    uint32_t prev = akira_input_get_bitmask();
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns = akira_input_get_bitmask(), just = btns & ~prev;
        prev = btns;
        if (!just) continue;

        if (just & BIT(AKIRA_BTN_HOME)) return false;
        if (just & BIT(AKIRA_BTN_UP))    { kb_move(-1, 0); draw_keyboard(ssid); }
        if (just & BIT(AKIRA_BTN_DOWN))  { kb_move(1, 0);  draw_keyboard(ssid); }
        if (just & BIT(AKIRA_BTN_LEFT))  { kb_move(0, -1); draw_keyboard(ssid); }
        if (just & BIT(AKIRA_BTN_RIGHT)) { kb_move(0, 1);  draw_keyboard(ssid); }
        if (just & BIT(AKIRA_BTN_A)) {
            if (g_kb_len < maxlen - 1) {
                char ch = (g_kb_row == 4) ? ' '
                    : (g_kb_shift ? KB_ROWS_UPPER[g_kb_row][g_kb_col]
                                  : KB_ROWS_LOWER[g_kb_row][g_kb_col]);
                g_kb_buf[g_kb_len++] = ch;
                draw_keyboard(ssid);
            }
        }
        if (just & BIT(AKIRA_BTN_B)) {
            if (g_kb_len > 0) {
                g_kb_buf[--g_kb_len] = 0;
                draw_keyboard(ssid);
            } else {
                return false;
            }
        }
        if (just & BIT(AKIRA_BTN_Y)) {
            g_kb_shift = !g_kb_shift;
            draw_keyboard(ssid);
        }
        if (just & BIT(AKIRA_BTN_X)) {
            memcpy(out, g_kb_buf, g_kb_len);
            out[g_kb_len] = 0;
            return true;
        }
    }
}

/* ---- Connect/scan popup (matches akira_os_shell.c's SD-card popup style) ---- */
#define WIFI_POPUP_W 240
#define WIFI_POPUP_H 70
#define WIFI_POPUP_X ((SCR_W - WIFI_POPUP_W) / 2)
#define WIFI_POPUP_Y ((SCR_H - WIFI_POPUP_H) / 2)
#define WIFI_POPUP_DOT_TICKS 15 /* advance dots every 300ms (15 * 20ms) */

static void draw_wifi_popup(const char *label, int dots) {
    int px = WIFI_POPUP_X, py = WIFI_POPUP_Y;
    akira_display_rect(px, py, WIFI_POPUP_W, WIFI_POPUP_H, C_BLACK);
    akira_display_rect_outline(px, py, WIFI_POPUP_W, WIFI_POPUP_H, C_WHITE);
    akira_display_rect_outline(px + 1, py + 1, WIFI_POPUP_W - 2, WIFI_POPUP_H - 2, C_WHITE);
    akira_display_text(px + 8, py + 8, "WiFi", C_WHITE);
    akira_display_hline(px + 8, py + 20, WIFI_POPUP_W - 16, C_WHITE);
    char line[24];
    snprintf(line, sizeof(line), "%s%.*s", label, dots, "...");
    akira_display_text(px + 8, py + 30, line, C_WHITE);
    akira_display_flush();
}

/* Polls sem in short slices so the popup animates instead of the UI thread
 * sitting on one long k_sem_take with a static label underneath it. */
static bool wait_with_popup(struct k_sem *sem, int64_t timeout_ms, const char *label) {
    int64_t deadline = k_uptime_get() + timeout_ms;
    int dots = 0, tick = 0;
    draw_wifi_popup(label, dots);
    while (k_uptime_get() < deadline) {
        if (k_sem_take(sem, K_MSEC(20)) == 0) {
            return true;
        }
        if (++tick >= WIFI_POPUP_DOT_TICKS) {
            tick = 0;
            dots = (dots + 1) % 4;
            draw_wifi_popup(label, dots);
        }
    }
    return false;
}

static bool do_connect(const char *ssid, const char *pass) {
    LOG_INF("WiFi connect SSID:%s",ssid);
    ensure_wifi_mgr_cb();

    /* wifi_manager_connect() refuses with -EALREADY while CONNECTED/CONNECTING
     * to a prior network — switching networks needs an explicit disconnect
     * first, or every attempt to join a different SSID fails outright. */
    wifi_mgr_state_t state = wifi_manager_get_state();
    if (state == WIFI_MGR_STATE_CONNECTED || state == WIFI_MGR_STATE_CONNECTING) {
        k_sem_reset(&g_disconnect_done);
        if (wifi_manager_disconnect() == 0) {
            wait_with_popup(&g_disconnect_done, 5000, "Disconnecting");
        }
    }

    /* wifi_manager_connect() always uses whatever is currently saved, so the
     * new credentials must be written before attempting to connect. Snapshot
     * what was there first so a failed attempt can't clobber a previously
     * good saved password for another network. */
    char prev_ssid[33]={0}, prev_psk[PASS_MAX]={0};
    bool had_prev = wifi_manager_get_saved_credentials(prev_ssid,sizeof(prev_ssid),
                                                        prev_psk,sizeof(prev_psk))==0;

    int r = wifi_manager_update_credentials(ssid, pass);
    if (r < 0) {
        LOG_ERR("update_credentials:%d", r);
        return false;
    }

    k_sem_reset(&g_connect_done);
    r = wifi_manager_connect();
    if (r < 0) {
        LOG_ERR("wifi_manager_connect:%d", r);
        if (had_prev) wifi_manager_update_credentials(prev_ssid, prev_psk);
        return false;
    }

    if (!wait_with_popup(&g_connect_done, 15000, "Connecting")) {
        LOG_WRN("WiFi connect timed out");
        if (had_prev) wifi_manager_update_credentials(prev_ssid, prev_psk);
        return false;
    }
    if (g_connect_result == 0 && had_prev) {
        wifi_manager_update_credentials(prev_ssid, prev_psk);
    }
    return g_connect_result != 0;
}

/* Returns false if the user cancelled the scan with HOME/B, true otherwise
 * (whether the scan completed, failed to start, or timed out). */
static bool do_scan(void) {
    g_network_count = 0;
    ensure_wifi_mgr_cb();

    k_sem_reset(&g_scan_done);
    int r = wifi_manager_scan();
    if (r < 0) {
        LOG_ERR("wifi_manager_scan:%d", r);
        return true;
    }

    int64_t deadline = k_uptime_get() + 10000;
    uint32_t prev = akira_input_get_bitmask();
    int dots = 0, tick = 0;
    draw_wifi_popup("Scanning", dots);
    while (k_uptime_get() < deadline) {
        if (k_sem_take(&g_scan_done, K_MSEC(20)) == 0) {
            size_t count = 0;
            wifi_manager_get_scan_results(g_networks, WIFI_MAX_NETWORKS, &count);
            g_network_count = (int)count;
            return true;
        }
        if (++tick >= WIFI_POPUP_DOT_TICKS) {
            tick = 0;
            dots = (dots + 1) % 4;
            draw_wifi_popup("Scanning", dots);
        }
        uint32_t btns = akira_input_get_bitmask();
        uint32_t just = btns & ~prev;
        prev = btns;
        if (just & (BIT(AKIRA_BTN_HOME) | BIT(AKIRA_BTN_B))) {
            return false;
        }
    }
    LOG_WRN("WiFi scan timed out");
    return true;
}

static bool get_connected_ssid(char *out, size_t out_sz) {
    if (wifi_manager_get_state() != WIFI_MGR_STATE_CONNECTED) {
        return false;
    }
    wifi_mgr_stats_t stats;
    if (wifi_manager_get_stats(&stats) != 0) {
        return false;
    }
    snprintf(out, out_sz, "%s", stats.ssid);
    return true;
}

static void build_idle_status(char *buf, size_t sz) {
    char ssid[33];
    if (get_connected_ssid(ssid, sizeof(ssid))) {
        snprintf(buf, sz, "Connected: %s", ssid);
    } else if (g_network_count) {
        snprintf(buf, sz, "Tap A to connect");
    } else {
        snprintf(buf, sz, "No networks found");
    }
}

static void draw_signal_lock(int idx, int sel, int8_t rssi, bool secured) {
    bool hi = (idx == sel);
    int iy = LIST_Y + idx * ITEM_H;
    uint16_t on  = hi ? C_BLACK : C_WHITE;
    uint16_t off = hi ? C_GRAY  : C_DKGRAY;

    int bars = (rssi >= -55) ? 4 : (rssi >= -65) ? 3 : (rssi >= -75) ? 2 : 1;
    int bars_right = ITEM_X + ITEM_W - 8;
    int bars_left  = bars_right - 22;
    int base_y = iy + 3 + (ITEM_H - 6) / 2 + 5;
    for (int i = 0; i < 4; i++) {
        int bh = 3 + i * 3;
        akira_display_rect(bars_left + i * 6, base_y - bh, 4, bh, (i < bars) ? on : off);
    }

    if (secured) {
        int lock_right = bars_left - 6;
        int lock_left  = lock_right - 10;
        int lock_top   = iy + 3 + (ITEM_H - 6) / 2 - 5;
        akira_display_rect_outline(lock_left, lock_top + 3, 10, 7, on);
        akira_display_rect(lock_left + 3, lock_top, 4, 5, on);
    }
}

static void draw_list(const char *status) {
    akira_display_clear(C_BLACK); lc_header("WIFI");
    akira_display_text(ITEM_X+4,LIST_Y+4,status,C_GRAY);
    int vis=(FOOT_Y-LIST_Y-18)/ITEM_H;
    if (g_scroll>g_network_count-vis) g_scroll=g_network_count>vis?g_network_count-vis:0;
    if (g_scroll<0) g_scroll=0;
    for (int i=g_scroll;i<g_network_count&&i<g_scroll+vis;i++) {
        lc_item(i-g_scroll,g_sel-g_scroll,g_networks[i].ssid,"");
        draw_signal_lock(i-g_scroll, g_sel-g_scroll, g_networks[i].rssi,
                          g_networks[i].security != 0 /* 0 == WIFI_SECURITY_TYPE_NONE */);
    }
    lc_footer("A-Connect  |  B-Back");
    akira_display_flush();
}

void wifi_screen_load(void)
{
    extern void settings_screen_load(void);
    g_sel=0; g_scroll=0;
    draw_list("Scanning...");
    if (!do_scan()) {
        settings_screen_load();
        return;
    }
    char idle_status[48];
    build_idle_status(idle_status, sizeof(idle_status));
    draw_list(idle_status);
    uint32_t prev=akira_input_get_bitmask();
    while (true) {
        k_sleep(K_MSEC(20));
        uint32_t btns=akira_input_get_bitmask(), just=btns&~prev; prev=btns;
        if (!just) continue;
        int vis=(FOOT_Y-LIST_Y-18)/ITEM_H;
        if (just&BIT(AKIRA_BTN_UP)) { if(g_sel>0){g_sel--;if(g_sel<g_scroll)g_scroll--;draw_list("Select network");} }
        if (just&BIT(AKIRA_BTN_DOWN)) { if(g_sel<g_network_count-1){g_sel++;if(g_sel>=g_scroll+vis)g_scroll++;draw_list("Select network");} }
        if ((just&BIT(AKIRA_BTN_A)) && g_network_count>0) {
            char pass[PASS_MAX]={0};
            char saved_ssid[33]={0}, saved_psk[PASS_MAX]={0};
            const char *initial_psk = NULL;
            /* Settings/NVS is a cache of the last saved connection — if it
             * matches the selected network, offer its PSK as a starting point. */
            if (wifi_manager_get_saved_credentials(saved_ssid,sizeof(saved_ssid),saved_psk,sizeof(saved_psk))==0
                && strcmp(saved_ssid,g_networks[g_sel].ssid)==0) {
                initial_psk = saved_psk;
            }
            if (enter_password(g_networks[g_sel].ssid,pass,PASS_MAX,initial_psk)) {
                bool ok = do_connect(g_networks[g_sel].ssid,pass);
                draw_list(ok ? "Connected!" : "Failed");
                k_sleep(K_MSEC(1200));
                build_idle_status(idle_status, sizeof(idle_status));
                draw_list(idle_status);
            } else {
                draw_list("Cancelled");
            }
            /* enter_password() ran its own blocking poll loop with its own
             * edge tracker; reseed ours so a still-held button (e.g. HOME)
             * isn't misread as a fresh press on the next tick. */
            prev = akira_input_get_bitmask();
        }
        if ((just&BIT(AKIRA_BTN_B))||(just&BIT(AKIRA_BTN_HOME))) { settings_screen_load(); return; }
    }
}
