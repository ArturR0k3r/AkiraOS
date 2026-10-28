/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#define LOG_MODULE_NAME akira_wifi_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_wifi_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file wifi_screen.c
 * @brief Settings → Wi-Fi: scan, join with the on-screen keyboard, details.
 *
 * Four pieces on the shell's screen stack, none of which blocks:
 *   wifi_list_screen — scan results; the scan is a state polled in tick()
 *   wifi_kb_screen   — QWERTY password entry for the network being joined
 *   wifi_busy_screen — modal "Connecting / Disconnecting" card that drives
 *                      the disconnect → save → connect sequence from tick()
 *   g_net_page       — list_screen page for the network we're connected to
 *
 * wifi_manager reports progress through one registered callback that only
 * gives semaphores; the screens poll them with K_NO_WAIT.
 */

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "shell_theme.h"
#include "ui/akira_ui.h"
#include "ui/anim.h"
#include "ui/list_screen.h"
#include "ui/overlay.h"
#include "ui/screen.h"
#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include "wifi_screen.h"

#include <connectivity/wifi/wifi_manager.h>

/* Network list: 6 rows fit the content band, with a scrollbar lane on the
 * right (same geometry as the generic list screen). */
#define ROW_H        30
#define ROW_TOP      (SCREEN_CONTENT_Y + 1)
#define ROW_X        12
#define ROW_W        (SCR_W - 30)
#define CARD_H       (ROW_H - 6)
#define VISIBLE_ROWS ((SCREEN_CONTENT_B - ROW_TOP) / ROW_H)
#define LIST_B       (ROW_TOP + VISIBLE_ROWS * ROW_H)
#define SEL_MS       130

#define SCAN_TIMEOUT_MS       10000
#define DISCONNECT_TIMEOUT_MS 5000
#define CONNECT_TIMEOUT_MS    15000
#define SPIN_STEP_MS          90   /* akira_ui_spinner() advances every 90 ms */

#define SSID_MAX 33
#define PASS_MAX 64

#define WIFI_MAX_NETWORKS 16
static wifi_mgr_scan_result_t g_networks[WIFI_MAX_NETWORKS] __attribute__((section(".ext_ram.bss")));
static int g_network_count;

/* Strings that outlive one callback.  PSRAM: internal DRAM is nearly full and
 * wifi_mgr_stats_t alone carries a MAX_VALUE_LEN SSID buffer. */
typedef struct {
    char ssid[SSID_MAX];        /* network being joined / disconnected */
    char psk[PASS_MAX];         /* password being typed / joined with */
    char prev_ssid[SSID_MAX];   /* saved credentials before the attempt */
    char prev_psk[PASS_MAX];
    bool had_prev;
    char linked[SSID_MAX];      /* SSID we're connected to, "" if none */
    wifi_mgr_stats_t stats;     /* details page cache */
} wifi_ctx_t;
static wifi_ctx_t g_ctx __attribute__((section(".ext_ram.bss")));

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

/* ---- small helpers ---- */

/* Copy @p s into @p out, cut to @p max_chars with a ".." tail if longer. */
static void fit_text(char *out, size_t out_sz, const char *s, int max_chars)
{
    snprintf(out, out_sz, "%s", s);
    max_chars = MIN(max_chars, (int)out_sz - 1);
    if (max_chars > 2 && (int)strlen(out) > max_chars) {
        out[max_chars - 2] = '.';
        out[max_chars - 1] = '.';
        out[max_chars] = '\0';
    }
}

static bool spin_advanced(int64_t *phase)
{
    int64_t p = k_uptime_get() / SPIN_STEP_MS;

    if (p != *phase) {
        *phase = p;
        return true;
    }
    return false;
}

static bool get_connected_ssid(char *out, size_t out_sz)
{
    if (wifi_manager_get_state() != WIFI_MGR_STATE_CONNECTED) {
        return false;
    }
    if (wifi_manager_get_stats(&g_ctx.stats) != 0) {
        return false;
    }
    snprintf(out, out_sz, "%.32s", g_ctx.stats.ssid); /* SSIDs are <= 32 bytes */
    return true;
}

/* @return true if the connected network changed. */
static bool refresh_linked(void)
{
    char ssid[SSID_MAX] = "";

    get_connected_ssid(ssid, sizeof(ssid));
    if (strcmp(ssid, g_ctx.linked) == 0) {
        return false;
    }
    snprintf(g_ctx.linked, sizeof(g_ctx.linked), "%s", ssid);
    return true;
}

static bool is_linked(const wifi_mgr_scan_result_t *n)
{
    return g_ctx.linked[0] && strcmp(n->ssid, g_ctx.linked) == 0;
}

/* wifi_manager writes credentials to NVS.  Hand it internal-RAM copies, not
 * pointers into g_ctx: a flash write disables the cache PSRAM is read
 * through. */
static int save_credentials(const char *ssid, const char *psk)
{
    char s[SSID_MAX], p[PASS_MAX];

    snprintf(s, sizeof(s), "%s", ssid);
    snprintf(p, sizeof(p), "%s", psk);
    return wifi_manager_update_credentials(s, p);
}

/* ================================================================== */
/* Connect / disconnect progress (modal, sticky)                       */
/* ================================================================== */

typedef enum { OP_CONNECT, OP_DISCONNECT } wifi_op_t;

typedef enum {
    PH_START,         /* first frame not painted yet */
    PH_DISCONNECTING, /* waiting for g_disconnect_done */
    PH_CONNECTING,    /* waiting for g_connect_done */
    PH_DONE,          /* result ready; popped on the next tick */
} busy_phase_t;

typedef enum {
    RES_CONNECTED,
    RES_FAILED,
    RES_TIMEOUT,
    RES_SAVE_FAILED,
    RES_DISCONNECTED,
    RES_DISCONNECT_FAILED,
} busy_result_t;

static struct {
    wifi_op_t op;
    busy_phase_t phase;
    busy_result_t result;
    int64_t deadline;
    int64_t spin_phase;
    bool painted;
} g_busy;

static void busy_done(busy_result_t res)
{
    g_busy.result = res;
    g_busy.phase = PH_DONE;
}

static void restore_prev_credentials(void)
{
    if (g_ctx.had_prev) {
        save_credentials(g_ctx.prev_ssid, g_ctx.prev_psk);
    }
}

static void busy_begin_connect(void)
{
    char ssid[SSID_MAX] = "", psk[PASS_MAX] = "";

    /* wifi_manager_connect() always uses whatever is currently saved, so the
     * new credentials must be written before attempting to connect. Snapshot
     * what was there first so a failed attempt can't clobber a previously
     * good saved password for another network. */
    g_ctx.had_prev = wifi_manager_get_saved_credentials(ssid, sizeof(ssid),
                                                        psk, sizeof(psk)) == 0;
    snprintf(g_ctx.prev_ssid, sizeof(g_ctx.prev_ssid), "%s", ssid);
    snprintf(g_ctx.prev_psk, sizeof(g_ctx.prev_psk), "%s", psk);

    LOG_INF("WiFi connect SSID:%s", g_ctx.ssid);
    int r = save_credentials(g_ctx.ssid, g_ctx.psk);
    if (r < 0) {
        LOG_ERR("update_credentials:%d", r);
        busy_done(RES_SAVE_FAILED);
        return;
    }

    k_sem_reset(&g_connect_done);
    r = wifi_manager_connect();
    if (r < 0) {
        LOG_ERR("wifi_manager_connect:%d", r);
        restore_prev_credentials();
        busy_done(RES_FAILED);
        return;
    }
    g_busy.phase = PH_CONNECTING;
    g_busy.deadline = k_uptime_get() + CONNECT_TIMEOUT_MS;
}

static void busy_start(void)
{
    wifi_mgr_state_t state = wifi_manager_get_state();

    /* wifi_manager_connect() refuses with -EALREADY while CONNECTED/CONNECTING
     * to a prior network — switching networks needs an explicit disconnect
     * first, or every attempt to join a different SSID fails outright. */
    if (g_busy.op == OP_DISCONNECT ||
        state == WIFI_MGR_STATE_CONNECTED || state == WIFI_MGR_STATE_CONNECTING) {
        k_sem_reset(&g_disconnect_done);
        if (wifi_manager_disconnect() == 0) {
            g_busy.phase = PH_DISCONNECTING;
            g_busy.deadline = k_uptime_get() + DISCONNECT_TIMEOUT_MS;
            return;
        }
        if (g_busy.op == OP_DISCONNECT) {
            busy_done(wifi_manager_get_state() == WIFI_MGR_STATE_IDLE
                          ? RES_DISCONNECTED : RES_DISCONNECT_FAILED);
            return;
        }
    }
    busy_begin_connect();
}

static void busy_finish(void)
{
    busy_result_t res = g_busy.result;

    memset(g_ctx.psk, 0, sizeof(g_ctx.psk));
    memset(g_ctx.prev_psk, 0, sizeof(g_ctx.prev_psk));
    screen_pop(); /* back to the network list, which refreshes on resume */

    switch (res) {
    case RES_CONNECTED:
        overlay_toast(TOAST_ICON_OK, "Connected", g_ctx.ssid, 2000);
        break;
    case RES_FAILED:
        overlay_toast(TOAST_ICON_ERROR, "Couldn't connect", g_ctx.ssid, 2500);
        break;
    case RES_TIMEOUT:
        overlay_toast(TOAST_ICON_ERROR, "Connection timed out", g_ctx.ssid, 2500);
        break;
    case RES_SAVE_FAILED:
        overlay_toast(TOAST_ICON_ERROR, "Couldn't save network", NULL, 2500);
        break;
    case RES_DISCONNECTED:
        overlay_toast(TOAST_ICON_INFO, "Disconnected", g_ctx.ssid, 2000);
        break;
    case RES_DISCONNECT_FAILED:
        overlay_toast(TOAST_ICON_ERROR, "Disconnect failed", NULL, 2500);
        break;
    }
}

static bool busy_tick(void)
{
    switch (g_busy.phase) {
    case PH_START:
        /* Let the card paint once before the (blocking) NVS write. */
        if (g_busy.painted) {
            busy_start();
            return true;
        }
        return false;

    case PH_DISCONNECTING: {
        bool done = k_sem_take(&g_disconnect_done, K_NO_WAIT) == 0;

        if (!done && k_uptime_get() < g_busy.deadline) {
            break;
        }
        if (g_busy.op == OP_CONNECT) {
            /* Timed out or not, try the new network (as before). */
            busy_begin_connect();
        } else {
            busy_done(done || wifi_manager_get_state() == WIFI_MGR_STATE_IDLE
                          ? RES_DISCONNECTED : RES_DISCONNECT_FAILED);
        }
        return true;
    }

    case PH_CONNECTING:
        if (k_sem_take(&g_connect_done, K_NO_WAIT) == 0) {
            if (g_connect_result) {
                busy_done(RES_CONNECTED);
            } else {
                restore_prev_credentials();
                busy_done(RES_FAILED);
            }
            return true;
        }
        if (k_uptime_get() >= g_busy.deadline) {
            LOG_WRN("WiFi connect timed out");
            restore_prev_credentials();
            busy_done(RES_TIMEOUT);
            return true;
        }
        break;

    case PH_DONE:
        busy_finish();
        return true;
    }
    return spin_advanced(&g_busy.spin_phase);
}

static void busy_draw(void)
{
    const int w = 240, h = 72;
    int x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;
    bool disc = g_busy.op == OP_DISCONNECT || g_busy.phase == PH_DISCONNECTING;
    char name[SSID_MAX];

    akira_ui_dither_card(x, y, w, h, 12, false, 4);
    akira_ui_spinner(x + 32, y + h / 2, 12, k_uptime_get());
    akira_display_text(x + 58, y + 20, disc ? "Disconnecting" : "Connecting",
                       AKIRA_UI_PAPER);
    fit_text(name, sizeof(name), g_ctx.ssid, (w - 58 - 12) / AKIRA_UI_CELL_W);
    akira_display_text(x + 58, y + 40, name, SHELL_C_GRAY);
    g_busy.painted = true;
}

static void busy_enter(void *arg)
{
    memset(&g_busy, 0, sizeof(g_busy));
    g_busy.op = (wifi_op_t)(uintptr_t)arg;
    g_busy.phase = PH_START;
    ensure_wifi_mgr_cb();
}

static void busy_leave(void)
{
    if (g_busy.phase == PH_DISCONNECTING || g_busy.phase == PH_CONNECTING) {
        /* Only reachable through screen_close() from below — STICKY keeps
         * HOME from popping us.  Saved credentials stay as they are. */
        LOG_WRN("WiFi progress closed mid-operation");
    }
}

/* Modal over the network list (no keys: the old popup ignored input too).
 * STICKY so a HOME long-press can't abandon an attempt halfway, after the
 * new password is saved but before a failure could restore the old one. */
static const shell_screen_t wifi_busy_screen = {
    .name = "wifi_busy",
    .flags = SCR_F_MODAL | SCR_F_STICKY,
    .enter = busy_enter,
    .leave = busy_leave,
    .tick = busy_tick,
    .draw = busy_draw,
};

static void connect_open(void)
{
    screen_push(&wifi_busy_screen, (void *)(uintptr_t)OP_CONNECT);
}

/* ================================================================== */
/* On-screen QWERTY keyboard for password entry                        */
/* ================================================================== */

static const char *const KB_ROWS_LOWER[4] = {"1234567890","qwertyuiop","asdfghjkl","zxcvbnm"};
static const char *const KB_ROWS_UPPER[4] = {"!@#$%^&*()","QWERTYUIOP","ASDFGHJKL","ZXCVBNM"};
static const int KB_ROW_LEN[5] = {10,10,9,7,1}; /* row 4 = space bar, 1 "column" */

#define KB_CELL_W   30
#define KB_ROW_H    28
#define KB_KEY_W    (KB_CELL_W - 3)
#define KB_KEY_H    (KB_ROW_H - 5)
#define KB_MARGIN_X ((SCR_W - 10 * KB_CELL_W) / 2)
#define KB_FIELD_Y  (SCREEN_CONTENT_Y + 16)
#define KB_FIELD_W  (10 * KB_CELL_W - 3)
#define KB_FIELD_H  20
#define KB_Y0       (KB_FIELD_Y + KB_FIELD_H + 6)

static struct {
    bool shift;
    bool joining;     /* X pressed: the busy screen takes g_ctx.psk over */
    int row, col;
    int len;          /* characters in g_ctx.psk */
} g_kb;

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
        g_kb.row = (g_kb.row + dr + 5) % 5;
        if (g_kb.col >= KB_ROW_LEN[g_kb.row]) {
            g_kb.col = KB_ROW_LEN[g_kb.row] - 1;
        }
    }
    if (dc != 0) {
        int len = KB_ROW_LEN[g_kb.row];
        g_kb.col = (g_kb.col + dc + len) % len;
    }
}

static void kb_update_hints(void)
{
    /* B deletes; on an empty field it backs out to the list. */
    screen_set_hints(g_kb.len ? "A:Type|B:Del" : "A:Type|B:Back", "Y:Shift|X:Connect");
}

static void kb_draw_key(int row, int col, bool hi)
{
    int y = KB_Y0 + row * KB_ROW_H;
    int x = kb_row_x0(row) + col * KB_CELL_W;
    int w = (row == 4) ? KB_CELL_W * 6 - 3 : KB_KEY_W;
    uint16_t fg = hi ? AKIRA_UI_INK : AKIRA_UI_PAPER;
    char cs[2] = {0, 0};
    const char *label = cs;

    if (row == 4) {
        label = "SPACE";
    } else {
        cs[0] = g_kb.shift ? KB_ROWS_UPPER[row][col] : KB_ROWS_LOWER[row][col];
    }
    akira_ui_dither_card(x, y, w, KB_KEY_H, 4, hi, hi ? 2 : 1);
    akira_display_text(x + (w - akira_ui_text_w(label, AKIRA_UI_FONT_SMALL)) / 2,
                       y + (KB_KEY_H - 10) / 2, label, fg);
}

static void kb_draw(void)
{
    char line[48];

    snprintf(line, sizeof(line), "Network: %.27s", g_ctx.ssid);
    akira_display_text(KB_MARGIN_X, SCREEN_CONTENT_Y + 2, line, SHELL_C_GRAY);

    /* Password field: the tail of what's typed, then a caret. */
    int max_chars = (KB_FIELD_W - 16) / AKIRA_UI_CELL_W;
    const char *shown = g_ctx.psk + MAX(g_kb.len - max_chars, 0);
    int tx = KB_MARGIN_X + 6;

    akira_display_rounded_rect(KB_MARGIN_X, KB_FIELD_Y, KB_FIELD_W, KB_FIELD_H, 4,
                               AKIRA_UI_PAPER);
    akira_display_text(tx, KB_FIELD_Y + (KB_FIELD_H - 10) / 2, shown, AKIRA_UI_PAPER);
    akira_display_vline(tx + akira_ui_text_w(shown, AKIRA_UI_FONT_SMALL) + 1,
                        KB_FIELD_Y + 4, KB_FIELD_H - 8, AKIRA_UI_PAPER);

    /* Idle keys first, the selected one last so its shadow sits on top. */
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < KB_ROW_LEN[row]; col++) {
            if (row != g_kb.row || col != g_kb.col) {
                kb_draw_key(row, col, false);
            }
        }
    }
    kb_draw_key(g_kb.row, g_kb.col, true);
}

static void kb_key(uint32_t just)
{
    if (just & BIT(AKIRA_BTN_UP))    { kb_move(-1, 0); }
    if (just & BIT(AKIRA_BTN_DOWN))  { kb_move(1, 0);  }
    if (just & BIT(AKIRA_BTN_LEFT))  { kb_move(0, -1); }
    if (just & BIT(AKIRA_BTN_RIGHT)) { kb_move(0, 1);  }
    if (just & BIT(AKIRA_BTN_A)) {
        if (g_kb.len < PASS_MAX - 1) {
            char ch = (g_kb.row == 4) ? ' '
                : (g_kb.shift ? KB_ROWS_UPPER[g_kb.row][g_kb.col]
                              : KB_ROWS_LOWER[g_kb.row][g_kb.col]);
            g_ctx.psk[g_kb.len++] = ch;
            g_ctx.psk[g_kb.len] = '\0';
        }
    }
    if (just & BIT(AKIRA_BTN_B)) {
        if (g_kb.len > 0) {
            g_ctx.psk[--g_kb.len] = '\0';
        } else {
            screen_pop();
            return;
        }
    }
    if (just & BIT(AKIRA_BTN_Y)) {
        g_kb.shift = !g_kb.shift;
    }
    if (just & BIT(AKIRA_BTN_X)) {
        /* The password is already in g_ctx.psk; join with it. */
        g_kb.joining = true;
        screen_replace(&wifi_busy_screen, (void *)(uintptr_t)OP_CONNECT);
        return;
    }
    kb_update_hints();
    screen_invalidate();
}

static void kb_enter(void *arg)
{
    char saved_ssid[SSID_MAX] = "", saved_psk[PASS_MAX] = "";

    ARG_UNUSED(arg);
    memset(&g_kb, 0, sizeof(g_kb));
    g_kb.row = 1; /* start on the qwerty row */
    memset(g_ctx.psk, 0, sizeof(g_ctx.psk));

    /* Settings/NVS is a cache of the last saved connection — if it matches
     * the selected network, offer its PSK as a starting point. */
    if (wifi_manager_get_saved_credentials(saved_ssid, sizeof(saved_ssid),
                                           saved_psk, sizeof(saved_psk)) == 0 &&
        strcmp(saved_ssid, g_ctx.ssid) == 0) {
        snprintf(g_ctx.psk, sizeof(g_ctx.psk), "%s", saved_psk);
        g_kb.len = (int)strlen(g_ctx.psk);
    }
    kb_update_hints();
}

static void kb_leave(void)
{
    /* Backed out (B / HOME): don't keep the typed password around.  On X the
     * busy screen replaces us and still needs it (it wipes it when done). */
    if (!g_kb.joining) {
        memset(g_ctx.psk, 0, sizeof(g_ctx.psk));
    }
}

static const shell_screen_t wifi_kb_screen = {
    .name = "wifi_kb",
    .title = "Wi-Fi password",
    .flags = SCR_F_STATUSBAR | SCR_F_FOOTER,
    .hint_l = "A:Type|B:Back",
    .hint_r = "Y:Shift|X:Connect",
    .enter = kb_enter,
    .leave = kb_leave,
    .on_key = kb_key,
    .draw = kb_draw,
};

/* ================================================================== */
/* Connected-network details (generic list page)                       */
/* ================================================================== */

static int64_t g_stats_at;
static bool g_stats_ok;

/* Live stats, re-read at most once a second (the page repaints at 1 Hz). */
static const wifi_mgr_stats_t *net_stats(void)
{
    int64_t now = k_uptime_get();

    if (g_stats_at == 0 || now - g_stats_at >= 1000) {
        g_stats_at = now;
        g_stats_ok = wifi_manager_get_stats(&g_ctx.stats) == 0;
    }
    return g_stats_ok ? &g_ctx.stats : NULL;
}

static void net_page_enter(void)
{
    g_stats_at = 0; /* force a fresh read */
}

static bool net_connected(const list_item_t *it)
{
    ARG_UNUSED(it);
    return wifi_manager_get_state() == WIFI_MGR_STATE_CONNECTED;
}

static void val_net_ssid(const list_item_t *it, char *buf, size_t len)
{
    const wifi_mgr_stats_t *s = net_stats();

    ARG_UNUSED(it);
    snprintf(buf, len, "%.22s", s ? s->ssid : "Not connected");
}

static void val_net_ip(const list_item_t *it, char *buf, size_t len)
{
    const wifi_mgr_stats_t *s = net_stats();

    ARG_UNUSED(it);
    snprintf(buf, len, "%s", (s && s->ip_addr[0]) ? s->ip_addr : "--");
}

static void val_net_signal(const list_item_t *it, char *buf, size_t len)
{
    const wifi_mgr_stats_t *s = net_stats();

    ARG_UNUSED(it);
    if (s) {
        snprintf(buf, len, "%d dBm", s->rssi);
    } else {
        snprintf(buf, len, "--");
    }
}

static void val_net_channel(const list_item_t *it, char *buf, size_t len)
{
    const wifi_mgr_stats_t *s = net_stats();

    ARG_UNUSED(it);
    if (s) {
        snprintf(buf, len, "%u", s->channel);
    } else {
        snprintf(buf, len, "--");
    }
}

static void act_net_password(const list_item_t *it)
{
    const wifi_mgr_stats_t *s = net_stats();

    ARG_UNUSED(it);
    if (!s) {
        return;
    }
    /* Re-enter the password for this network, then reconnect with it. */
    snprintf(g_ctx.ssid, sizeof(g_ctx.ssid), "%.32s", s->ssid);
    screen_replace(&wifi_kb_screen, NULL);
}

static void act_net_disconnect(const list_item_t *it)
{
    const wifi_mgr_stats_t *s = net_stats();

    ARG_UNUSED(it);
    snprintf(g_ctx.ssid, sizeof(g_ctx.ssid), "%.32s", s ? s->ssid : g_ctx.linked);
    screen_replace(&wifi_busy_screen, (void *)(uintptr_t)OP_DISCONNECT);
}

static const list_item_t g_net_items[] = {
    { .kind = LI_INFO, .label = "Network", .value = val_net_ssid },
    { .kind = LI_INFO, .label = "IP address", .value = val_net_ip },
    { .kind = LI_INFO, .label = "Signal", .value = val_net_signal },
    { .kind = LI_INFO, .label = "Channel", .value = val_net_channel },
    { .kind = LI_ACTION, .label = "Change password", .action = act_net_password,
      .enabled = net_connected },
    { .kind = LI_ACTION, .label = "Disconnect", .action = act_net_disconnect,
      .enabled = net_connected },
};

static const list_model_t g_net_page = {
    .title = "Network",
    .items = g_net_items,
    .count = ARRAY_SIZE(g_net_items),
    .on_enter = net_page_enter,
    .live = true,
};

/* ================================================================== */
/* Network list                                                        */
/* ================================================================== */

typedef enum { WS_IDLE, WS_SCANNING } ws_state_t;

static struct {
    ws_state_t state;
    int64_t deadline;
    int64_t spin_phase;
    int sel;
    int scroll;          /* first visible row */
    anim_t hl;           /* highlight position, px from the list's origin */
    anim_t scroll_px;    /* scroll position, px */
} g_ws;

static void list_update_hints(void)
{
    if (g_ws.state == WS_SCANNING) {
        screen_set_hints(NULL, "B:Back");
    } else if (g_network_count == 0) {
        screen_set_hints("Y:Rescan", "B:Back");
    } else if (is_linked(&g_networks[g_ws.sel])) {
        screen_set_hints("A:Details|Y:Rescan", "B:Back");
    } else {
        screen_set_hints("A:Connect|Y:Rescan", "B:Back");
    }
}

/* Drop hidden networks (no SSID to join by), keep the strongest entry per
 * SSID (one per access point otherwise), strongest first. */
static int tidy_results(int n)
{
    int out = 0;

    for (int i = 0; i < n; i++) {
        const wifi_mgr_scan_result_t *r = &g_networks[i];
        int dup = -1;

        if (r->ssid[0] == '\0') {
            continue;
        }
        for (int j = 0; j < out; j++) {
            if (strcmp(g_networks[j].ssid, r->ssid) == 0) {
                dup = j;
                break;
            }
        }
        if (dup >= 0) {
            if (r->rssi > g_networks[dup].rssi) {
                g_networks[dup] = *r;
            }
            continue;
        }
        if (out != i) {
            g_networks[out] = *r;
        }
        out++;
    }
    for (int i = 1; i < out; i++) {
        wifi_mgr_scan_result_t t = g_networks[i];
        int j = i - 1;

        while (j >= 0 && g_networks[j].rssi < t.rssi) {
            g_networks[j + 1] = g_networks[j];
            j--;
        }
        g_networks[j + 1] = t;
    }
    return out;
}

static void scan_start(void)
{
    g_network_count = 0;
    g_ws.sel = 0;
    g_ws.scroll = 0;
    anim_set(&g_ws.hl, 0);
    anim_set(&g_ws.scroll_px, 0);

    k_sem_reset(&g_scan_done);
    int r = wifi_manager_scan();
    /* -EBUSY: a scan we walked away from earlier is still running — its
     * SCAN_DONE serves just as well. */
    if (r < 0 && r != -EBUSY) {
        LOG_ERR("wifi_manager_scan:%d", r);
        overlay_toast(TOAST_ICON_ERROR, "Wi-Fi scan failed", NULL, 2000);
        g_ws.state = WS_IDLE;
    } else {
        g_ws.state = WS_SCANNING;
        g_ws.deadline = k_uptime_get() + SCAN_TIMEOUT_MS;
    }
    list_update_hints();
    screen_invalidate();
}

/* @return true when the scan finished or gave up (repaint). */
static bool scan_poll(void)
{
    if (k_sem_take(&g_scan_done, K_NO_WAIT) == 0) {
        size_t count = 0;

        wifi_manager_get_scan_results(g_networks, WIFI_MAX_NETWORKS, &count);
        g_network_count = tidy_results((int)count);
        g_ws.state = WS_IDLE;
        refresh_linked();
        list_update_hints();
        return true;
    }
    if (k_uptime_get() >= g_ws.deadline) {
        LOG_WRN("WiFi scan timed out");
        overlay_toast(TOAST_ICON_ERROR, "Wi-Fi scan timed out", NULL, 2000);
        g_ws.state = WS_IDLE;
        list_update_hints();
        return true;
    }
    return false;
}

static int rssi_bars(int rssi)
{
    return (rssi >= -55) ? 4 : (rssi >= -65) ? 3 : (rssi >= -75) ? 2 : 1;
}

/* Four bars, tallest on the right, bottoms on @p base_y; how many are lit
 * follows the RSSI (the old lc_bar() meter filled its whole track whatever
 * the level). */
static void draw_signal_bars(int x, int base_y, int rssi, uint16_t color)
{
    int lit = rssi_bars(rssi);

    for (int i = 0; i < 4; i++) {
        int bh = 3 + i * 3;
        int bx = x + i * 6, by = base_y - bh;

        if (i < lit) {
            akira_display_rect(bx, by, 4, bh, color);
        } else {
            akira_display_rect_outline(bx, by, 4, bh, color);
        }
    }
}

/* 10x12 padlock, top-left at (x, y). */
static void draw_lock(int x, int y, uint16_t color)
{
    akira_display_rect_outline(x + 2, y, 6, 6, color);
    akira_display_rect(x, y + 5, 10, 7, color);
}

/* 10x8 check mark centred on y. */
static void draw_check(int x, int y, uint16_t color)
{
    for (int t = 0; t < 2; t++) {
        akira_display_line(x, y + t, x + 3, y + 3 + t, color);
        akira_display_line(x + 3, y + 3 + t, x + 9, y - 3 + t, color);
    }
}

static void draw_network(int i, int y, bool inverted)
{
    const wifi_mgr_scan_result_t *n = &g_networks[i];
    uint16_t fg = inverted ? AKIRA_UI_INK : AKIRA_UI_PAPER;
    int cy = y + CARD_H / 2;
    int x = ROW_X + ROW_W - 10 - 22; /* left edge of the signal bars */
    char name[SSID_MAX];

    draw_signal_bars(x, cy + 6, n->rssi, fg);
    if (n->security != 0 /* 0 == WIFI_SECURITY_TYPE_NONE */) {
        x -= 16;
        draw_lock(x, cy - 6, fg);
    }
    if (is_linked(n)) {
        x -= 16;
        draw_check(x, cy, fg);
    }
    fit_text(name, sizeof(name), n->ssid, (x - 8 - (ROW_X + 12)) / AKIRA_UI_CELL_W);
    akira_display_text(ROW_X + 12, y + (CARD_H - 10) / 2, name, fg);
}

static void draw_centred(int y, const char *s, uint16_t color)
{
    akira_display_text((SCR_W - akira_ui_text_w(s, AKIRA_UI_FONT_SMALL)) / 2, y, s, color);
}

static void ws_draw(void)
{
    if (g_ws.state == WS_SCANNING) {
        akira_ui_spinner(SCR_W / 2, 104, 14, k_uptime_get());
        draw_centred(130, "Scanning for networks", AKIRA_UI_PAPER);
        return;
    }
    if (g_network_count == 0) {
        draw_centred(104, "No networks found", AKIRA_UI_PAPER);
        draw_centred(122, "Press Y to scan again", SHELL_C_GRAY);
        return;
    }

    int scroll = anim_value(&g_ws.scroll_px);
    int hl = anim_value(&g_ws.hl);
    int first = MAX(scroll / ROW_H - 1, 0);
    int last = MIN(first + VISIBLE_ROWS + 2, g_network_count);

    /* Idle cards first, then the highlight over them, then the contents. */
    for (int i = first; i < last; i++) {
        akira_ui_dither_card(ROW_X, ROW_TOP + i * ROW_H - scroll, ROW_W, CARD_H, 8,
                             false, 2);
    }
    akira_ui_dither_card(ROW_X, ROW_TOP + hl - scroll, ROW_W, CARD_H, 8, true, 3);
    for (int i = first; i < last; i++) {
        draw_network(i, ROW_TOP + i * ROW_H - scroll, abs(hl - i * ROW_H) < ROW_H / 2);
    }

    /* Clip rows that are scrolling in or out above / below the list. */
    akira_display_rect(0, SBAR_H + 2, SCR_W, ROW_TOP - SBAR_H - 2, AKIRA_UI_INK);
    akira_display_rect(0, LIST_B, SCR_W, FOOT_Y - LIST_B, AKIRA_UI_INK);

    /* Scrollbar lane */
    if (g_network_count > VISIBLE_ROWS) {
        int track_y = ROW_TOP, track_h = VISIBLE_ROWS * ROW_H - 6;
        int max_scroll = (g_network_count - VISIBLE_ROWS) * ROW_H;
        int thumb_h = MAX(track_h * VISIBLE_ROWS / g_network_count, 12);
        int thumb_y = track_y + (track_h - thumb_h) * CLAMP(scroll, 0, max_scroll) / max_scroll;

        akira_display_vline(SCR_W - 9, track_y, track_h, SHELL_C_DKGRAY);
        akira_display_rounded_rect_fill(SCR_W - 11, thumb_y, 5, thumb_h, 2, AKIRA_UI_PAPER);
    }
}

static void ws_move_sel(int to)
{
    to = CLAMP(to, 0, g_network_count - 1);
    if (to == g_ws.sel) {
        return;
    }
    g_ws.sel = to;
    anim_to(&g_ws.hl, to * ROW_H, SEL_MS, ANIM_EASE_OUT);

    int scroll = g_ws.scroll;

    if (to < scroll) {
        scroll = to;
    } else if (to >= scroll + VISIBLE_ROWS) {
        scroll = to - VISIBLE_ROWS + 1;
    }
    if (scroll != g_ws.scroll) {
        g_ws.scroll = scroll;
        anim_to(&g_ws.scroll_px, scroll * ROW_H, SEL_MS + 40, ANIM_EASE_OUT);
    }
    list_update_hints();
    screen_invalidate();
}

static void open_network(int i)
{
    const wifi_mgr_scan_result_t *n = &g_networks[i];

    if (is_linked(n)) {
        list_screen_open(&g_net_page);
        return;
    }
    snprintf(g_ctx.ssid, sizeof(g_ctx.ssid), "%s", n->ssid);
    if (n->security == 0 /* WIFI_SECURITY_TYPE_NONE */) {
        /* Open network: nothing to type, join straight away. */
        memset(g_ctx.psk, 0, sizeof(g_ctx.psk));
        connect_open();
    } else {
        screen_push(&wifi_kb_screen, NULL);
    }
}

static void ws_key(uint32_t just)
{
    if (just & BIT(AKIRA_BTN_B)) {
        screen_pop(); /* also abandons a scan in progress */
        return;
    }
    if (g_ws.state == WS_SCANNING) {
        return;
    }
    if (just & BIT(AKIRA_BTN_Y)) {
        scan_start();
        return;
    }
    if (g_network_count == 0) {
        return;
    }
    if (just & BIT(AKIRA_BTN_UP)) {
        ws_move_sel(g_ws.sel - 1);
    }
    if (just & BIT(AKIRA_BTN_DOWN)) {
        ws_move_sel(g_ws.sel + 1);
    }
    if (just & BIT(AKIRA_BTN_A)) {
        open_network(g_ws.sel);
    }
}

static bool ws_tick(void)
{
    bool redraw = anim_running(&g_ws.hl) || anim_running(&g_ws.scroll_px);

    if (g_ws.state == WS_SCANNING) {
        if (scan_poll()) {
            return true;
        }
        if (spin_advanced(&g_ws.spin_phase)) {
            redraw = true;
        }
    }
    return redraw;
}

static void ws_status(void)
{
    /* The link can drop or come back (auto-reconnect) while we're open. */
    if (g_ws.state == WS_IDLE && refresh_linked()) {
        list_update_hints();
        screen_invalidate();
    }
}

static void ws_resume(void)
{
    /* Back from joining / details: the connected network may have changed. */
    refresh_linked();
    list_update_hints();
}

static void ws_enter(void *arg)
{
    ARG_UNUSED(arg);
    ensure_wifi_mgr_cb();
    memset(&g_ws, 0, sizeof(g_ws));
    refresh_linked();
    scan_start();
}

static void ws_leave(void)
{
    g_ws.state = WS_IDLE;
}

static const shell_screen_t wifi_list_screen = {
    .name = "wifi",
    .title = "Wi-Fi",
    .flags = SCR_F_STATUSBAR | SCR_F_FOOTER,
    .hint_l = NULL,
    .hint_r = "B:Back",
    .enter = ws_enter,
    .leave = ws_leave,
    .resume = ws_resume,
    .on_key = ws_key,
    .tick = ws_tick,
    .draw = ws_draw,
    .on_status = ws_status,
};

void wifi_open(void)
{
    screen_push(&wifi_list_screen, NULL);
}
