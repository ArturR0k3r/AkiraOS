/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_play_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_play_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akiraplay_screen.c
 * @brief AkiraPlay — App Store browsing the AkiraConsoleApp R2 catalog over
 *        WiFi. Same Liquid-Glass renderer as the Settings screens (pure
 *        akira_display_* calls, no LVGL).
 *
 * Flow: LOADING (fetch + parse catalogue.json) -> LIST (category tabs,
 * X cycles a tag filter, Y opens on-screen search) -> DETAIL (thumbnail +
 * tags, A to install) -> back to LIST. SEARCH is a D-pad letter grid that
 * applies a substring filter on display_name. ERROR on fetch failure (no
 * WiFi, timeout, bad JSON), with retry.
 */

#include "akiraplay_screen.h"
#include "home_screen.h"
#include "settings_shared.h"
#include "catalog_net.h"
#include "catalogue_parser.h"
#include "install_progress_screen.h"
#include "ui/akira_ui.h"

#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <runtime/app_manager/app_manager.h>
#include <connectivity/wifi/wifi_manager.h>

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#define PLAY_HOST "console.app.akiraos.dev"
#define PLAY_CATALOGUE_PATH "/catalog/catalogue.json"
#define PLAY_JSON_BUF_SIZE 16384
#define PLAY_PKG_BUF_SIZE (256 * 1024)
#define PLAY_FETCH_TIMEOUT_MS 15000
#define PLAY_DOWNLOAD_TIMEOUT_MS 30000

/* On-device thumbnail contract, matches AkiraConsoleApp's submit.ts/
 * SubmitApp.tsx: fixed 96x64 1bpp bitmap, MSB-first, rows byte-aligned
 * (96/8=12 B/row exactly, no padding). Kept 1-bit to match this UI kit's
 * dither-only palette and to keep the download tiny (768 B vs a full-color
 * image) — decoded to RGB565 only transiently, for the single blit call. */
#define THUMB_W 96
#define THUMB_H 64
#define THUMB_ROW_BYTES (THUMB_W / 8)
#define THUMB_PACKED_BYTES (THUMB_ROW_BYTES * THUMB_H)

static uint8_t g_json_buf[PLAY_JSON_BUF_SIZE] __attribute__((section(".ext_ram.bss")));
static uint8_t g_pkg_buf[PLAY_PKG_BUF_SIZE] __attribute__((section(".ext_ram.bss")));
static catalogue_entry_t g_entries[CATALOGUE_MAX_APPS] __attribute__((section(".ext_ram.bss")));
static uint8_t g_thumb_packed[THUMB_PACKED_BYTES] __attribute__((section(".ext_ram.bss")));
static uint16_t g_thumb_rgb[THUMB_W * THUMB_H] __attribute__((section(".ext_ram.bss")));

/* ensure_thumbnail()'s scratch — static rather than local: it's reached via
 * handle_list()->redraw()->draw_detail()->ensure_thumbnail(), one call frame
 * deeper than the fetch_catalogue()/do_install() paths to the same
 * stack-hungry TLS send/close code, on this 8192-byte UI thread
 * stack (SHELL_THREAD_STACK_SIZE) — every extra byte of on-stack locals at
 * this depth narrows an already-tight budget. */
static char g_thumb_url_host[128] __attribute__((section(".ext_ram.bss")));
static char g_thumb_url_path[256] __attribute__((section(".ext_ram.bss")));

typedef enum {
    PLAY_LOADING = 0,
    PLAY_LIST,
    PLAY_DETAIL,
    PLAY_SEARCH,
    PLAY_ERROR,
} play_state_t;

static play_state_t g_state;
static bool g_active;

static int g_count;                       /* total entries parsed */
static int g_filtered[CATALOGUE_MAX_APPS]; /* indices into g_entries */
static int g_filtered_count;
static int g_sel, g_scroll;
static int g_tab;
static int g_tag_filter = -1; /* -1 = all tags, else bit index into tag_mask */
static char g_search_query[CATALOGUE_NAME_LEN];
static int g_detail_idx; /* index into g_entries for the open detail page */
static char g_error_msg[64];

static bool g_thumb_valid;
static int g_thumb_idx = -1; /* g_entries index the loaded thumbnail belongs to */

#define TAB_COUNT 4
static const char *TAB_KEYS[TAB_COUNT]   = { "", "console_apps", "retro", "generic" };
static const char *TAB_LABELS[TAB_COUNT] = { "All", "Apps", "Retro", "Other" };

/* On-screen search keyboard: D-pad grid, no dedicated space/done cells —
 * those are covered by the physical X (backspace) / Y (clear) / B (apply)
 * buttons instead, so the grid only needs to hold letters. */
#define KB_COLS 7
#define KB_ROWS 4
static const char KB_LAYOUT[KB_ROWS][KB_COLS] = {
    { 'A', 'B', 'C', 'D', 'E', 'F', 'G' },
    { 'H', 'I', 'J', 'K', 'L', 'M', 'N' },
    { 'O', 'P', 'Q', 'R', 'S', 'T', 'U' },
    { 'V', 'W', 'X', 'Y', 'Z', '_', '_' }, /* '_' = unused cell */
};
static int g_kb_row, g_kb_col;

/* ------------------------------------------------------------------ */
/* Filtering                                                           */
/* ------------------------------------------------------------------ */
static bool str_ci_contains(const char *hay, const char *needle)
{
    size_t hn = strlen(hay), nn = strlen(needle);
    if (nn == 0) {
        return true;
    }
    if (nn > hn) {
        return false;
    }
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        for (; j < nn; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) {
                break;
            }
        }
        if (j == nn) {
            return true;
        }
    }
    return false;
}

static void rebuild_filter(void)
{
    const char *key = TAB_KEYS[g_tab];
    g_filtered_count = 0;
    for (int i = 0; i < g_count; i++) {
        if (key[0] != '\0' && strcmp(g_entries[i].category, key) != 0) {
            continue;
        }
        if (g_tag_filter >= 0 && !(g_entries[i].tag_mask & (1u << g_tag_filter))) {
            continue;
        }
        if (g_search_query[0] != '\0' && !str_ci_contains(g_entries[i].display_name, g_search_query)) {
            continue;
        }
        g_filtered[g_filtered_count++] = i;
    }
    g_sel = 0;
    g_scroll = 0;
}

/* ------------------------------------------------------------------ */
/* URL split — mirrors catalog_https_get()'s own redirect-URL parsing  */
/* ------------------------------------------------------------------ */
static int split_https_url(const char *url, char *host, size_t host_size,
                            char *path, size_t path_size)
{
    if (strncmp(url, "https://", 8) != 0) {
        return -EPROTO;
    }
    const char *p = url + 8;
    const char *slash = strchr(p, '/');
    if (!slash) {
        return -EPROTO;
    }
    size_t hlen = (size_t)(slash - p);
    if (hlen >= host_size) {
        hlen = host_size - 1;
    }
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    strncpy(path, slash, path_size - 1);
    path[path_size - 1] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* Network                                                             */
/* ------------------------------------------------------------------ */
/* Kept alive across the catalogue fetch and any subsequent install download
 * — TLS handshakes against PLAY_HOST routinely take ~15-20s on this
 * hardware (see catalog_net.h's catalog_https_open() doc comment), so
 * reusing one connection instead of opening a fresh one per request halves
 * how often that cost is paid. Dropped and reopened if a request finds it
 * stale (peer closed an idle connection while the user was browsing). */
static int g_session_handle = -1;

static int catalog_session_ensure(void)
{
    if (g_session_handle >= 0) {
        return g_session_handle;
    }
    g_session_handle = catalog_https_open(PLAY_HOST, 443, PLAY_FETCH_TIMEOUT_MS);
    return g_session_handle;
}

static void catalog_session_close(void)
{
    if (g_session_handle >= 0) {
        catalog_https_close(g_session_handle);
        g_session_handle = -1;
    }
}

static int fetch_catalogue(void)
{
    if (wifi_manager_get_state() != WIFI_MGR_STATE_CONNECTED) {
        return -ENOTCONN;
    }

    catalog_session_close(); /* fresh session each time the screen loads */
    int handle = catalog_session_ensure();
    if (handle < 0) {
        return handle;
    }

    int n = catalog_https_get_on(handle, PLAY_HOST, PLAY_CATALOGUE_PATH,
                                 g_json_buf, sizeof(g_json_buf) - 1,
                                 PLAY_FETCH_TIMEOUT_MS);
    if (n < 0) {
        catalog_session_close();
        return n;
    }
    g_json_buf[n] = '\0';

    g_count = catalogue_parse((const char *)g_json_buf, (size_t)n,
                              g_entries, CATALOGUE_MAX_APPS);
    if (g_count < 0) {
        return g_count;
    }
    g_thumb_idx = -1; /* g_entries was just overwritten, drop the stale cache */
    g_thumb_valid = false;
    return 0;
}

/* Fetches and decodes the thumbnail for g_entries[idx] on first access, then
 * caches by index — draw_detail() calls this every redraw but the network
 * round trip (on the already-open keep-alive session) only happens once per
 * catalogue load per app. */
static void ensure_thumbnail(int idx)
{
    if (g_thumb_idx == idx) {
        return;
    }
    g_thumb_idx = idx;
    g_thumb_valid = false;

    const catalogue_entry_t *e = &g_entries[idx];
    if (e->thumbnail_url[0] == '\0') {
        return;
    }

    if (split_https_url(e->thumbnail_url, g_thumb_url_host, sizeof(g_thumb_url_host),
                        g_thumb_url_path, sizeof(g_thumb_url_path)) < 0) {
        return;
    }
    /* g_thumb_url_host unused past this point — thumbnail_url is always
     * same-origin as PLAY_HOST */

    int handle = catalog_session_ensure();
    if (handle < 0) {
        return;
    }

    int n = catalog_https_get_on(handle, PLAY_HOST, g_thumb_url_path, g_thumb_packed,
                                 sizeof(g_thumb_packed), PLAY_FETCH_TIMEOUT_MS);
    if (n != (int)sizeof(g_thumb_packed)) {
        return; /* wrong size — corrupt or mismatched contract, skip silently */
    }

    for (int y = 0; y < THUMB_H; y++) {
        for (int x = 0; x < THUMB_W; x++) {
            uint8_t byte = g_thumb_packed[y * THUMB_ROW_BYTES + (x >> 3)];
            bool bit = (byte >> (7 - (x & 7))) & 1;
            /* bit=1 is PAPER (dark/foreground) — SS_C_WHITE is that role's
             * RGB565 value despite the name (see settings_shared.h). */
            g_thumb_rgb[y * THUMB_W + x] = bit ? SS_C_WHITE : SS_C_BLACK;
        }
    }
    g_thumb_valid = true;
}

static void set_error_from_errno(int err)
{
    switch (err) {
    case -ENOTCONN:
        strncpy(g_error_msg, "No WiFi connection", sizeof(g_error_msg) - 1);
        break;
    case -ETIMEDOUT:
        strncpy(g_error_msg, "Network timeout", sizeof(g_error_msg) - 1);
        break;
    case -EINVAL:
        strncpy(g_error_msg, "Catalogue data invalid", sizeof(g_error_msg) - 1);
        break;
    default:
        snprintf(g_error_msg, sizeof(g_error_msg), "Fetch failed (%d)", err);
        break;
    }
    g_error_msg[sizeof(g_error_msg) - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/* Draw                                                                 */
/* ------------------------------------------------------------------ */
static void draw_loading(void)
{
    akira_display_clear(SS_C_BLACK);
    ss_draw_header("AKIRA PLAY");
    ss_draw_centred(0, SS_CONT_Y + (SS_RIB_Y - SS_CONT_Y) / 2 - 5, SS_SCR_W,
                    "Loading catalogue...", SS_C_WHITE, SS_C_BLACK);
    ss_draw_ribbon("", "[B] Back");
    akira_display_flush();
}

static void draw_error(void)
{
    akira_display_clear(SS_C_BLACK);
    ss_draw_header("AKIRA PLAY");
    ss_draw_centred(0, SS_CONT_Y + (SS_RIB_Y - SS_CONT_Y) / 2 - 5, SS_SCR_W,
                    g_error_msg, SS_C_WHITE, SS_C_BLACK);
    ss_draw_ribbon("[A] Retry", "[B] Back");
    akira_display_flush();
}

static void draw_list_item(int row, bool hi, const catalogue_entry_t *e)
{
    int bx = SS_MENU_X;
    int by = SS_CONT_Y + 16 + row * SS_MENU_ITH + 2;
    int bw = SS_MENU_W;
    int bh = SS_MENU_ITH - 4;

    akira_ui_dither_card(bx, by, bw, bh, 8, hi, hi ? 3 : 2);
    int ty = by + (bh - 10) / 2;
    uint16_t fg = hi ? SS_C_BLACK : SS_C_WHITE;
    akira_display_text(bx + 10, ty, e->display_name, fg);

    char sz[16];
    if (e->size_bytes >= 1024) {
        snprintf(sz, sizeof(sz), "%uKB", (unsigned)((e->size_bytes + 1023) / 1024));
    } else {
        sz[0] = '\0';
    }
    if (sz[0]) {
        int len = (int)strlen(sz);
        akira_display_text(bx + bw - len * 8 - 10, ty, sz, fg);
    }
}

static void draw_list(void)
{
    akira_display_clear(SS_C_BLACK);

    char title[48];
    if (g_tag_filter >= 0 && g_search_query[0]) {
        snprintf(title, sizeof(title), "PLAY:%s|%s|\"%s\"",
                TAB_LABELS[g_tab], catalogue_tag_name(g_tag_filter), g_search_query);
    } else if (g_tag_filter >= 0) {
        snprintf(title, sizeof(title), "PLAY:%s|%s", TAB_LABELS[g_tab], catalogue_tag_name(g_tag_filter));
    } else if (g_search_query[0]) {
        snprintf(title, sizeof(title), "PLAY:%s|\"%s\"", TAB_LABELS[g_tab], g_search_query);
    } else {
        snprintf(title, sizeof(title), "PLAY: %s", TAB_LABELS[g_tab]);
    }
    ss_draw_header(title);

    int top_y = SS_CONT_Y + 16;
    int vis = ss_menu_vis_count(top_y);
    if (g_scroll > g_filtered_count - vis) g_scroll = g_filtered_count - vis;
    if (g_scroll < 0) g_scroll = 0;

    akira_display_rect(0, SS_CONT_Y, SS_SCR_W, SS_RIB_Y - SS_CONT_Y, SS_C_BLACK);

    if (g_filtered_count == 0) {
        int ty = SS_CONT_Y + (SS_RIB_Y - SS_CONT_Y - 10) / 2;
        const char *msg = "No apps in this category";
        int msg_len = (int)strlen(msg) * 8;
        akira_display_text((SS_SCR_W - msg_len) / 2, ty, msg, SS_C_GRAY);
    } else {
        for (int i = g_scroll; i < g_filtered_count && i < g_scroll + vis; i++) {
            draw_list_item(i - g_scroll, (i == g_sel), &g_entries[g_filtered[i]]);
        }
        if (g_scroll > 0) {
            akira_display_text(SS_SCR_W - 10, top_y + 2, "^", SS_C_GRAY);
        }
        if (g_scroll + vis < g_filtered_count) {
            akira_display_text(SS_SCR_W - 10, top_y + vis * SS_MENU_ITH + 2, "v", SS_C_GRAY);
        }
    }

    ss_draw_ribbon("[A] View <>Cat  X Tag", "[B] Back  Y Find");
    akira_display_flush();
}

static void draw_detail(void)
{
    const catalogue_entry_t *e = &g_entries[g_detail_idx];

    akira_display_clear(SS_C_BLACK);
    ss_draw_header("APP DETAIL");
    akira_display_rect(0, SS_CONT_Y, SS_SCR_W, SS_RIB_Y - SS_CONT_Y, SS_C_BLACK);

    int thumb_x = (SS_SCR_W - THUMB_W) / 2;
    int thumb_y = SS_CONT_Y + 4;

    /* Static, not a local — draw_detail() sits directly above
     * ensure_thumbnail()'s TLS call below on this thread's 8192-byte stack;
     * see g_thumb_url_host's comment. */
    static char lines[4][48];
    snprintf(lines[0], sizeof(lines[0]), "%s", e->display_name);
    snprintf(lines[1], sizeof(lines[1]), "Version: %s", e->version);
    snprintf(lines[2], sizeof(lines[2]), "Category: %s", e->category);
    if (e->size_bytes >= 1024) {
        snprintf(lines[3], sizeof(lines[3]), "Size: %uKB", (unsigned)((e->size_bytes + 1023) / 1024));
    } else {
        snprintf(lines[3], sizeof(lines[3]), "Size: %u B", (unsigned)e->size_bytes);
    }

    bool installed = (app_manager_get_state(e->name) != APP_STATE_NEW);

    int sy = thumb_y + THUMB_H + 8;
    for (int i = 0; i < 4; i++) {
        akira_display_text(20, sy + i * 16, lines[i], SS_C_WHITE);
    }

    /* Tag badges, wrapping to a second row; capped at 6 to keep a bounded
     * worst-case height (this area only has ~40px before the ribbon). */
    int tag_x = 20, tag_y = sy + 4 * 16 + 4;
    int shown = 0;
    for (int i = 0; i < CATALOGUE_TAG_COUNT && shown < 6; i++) {
        if (!(e->tag_mask & (1u << i))) {
            continue;
        }
        const char *name = catalogue_tag_name(i);
        int w = (int)strlen(name) * 8 + 10;
        if (tag_x + w > SS_SCR_W - 10) {
            tag_x = 20;
            tag_y += 20;
        }
        akira_ui_tag(tag_x, tag_y, name, false);
        tag_x += w + 4;
        shown++;
    }

    if (installed) {
        akira_display_text(20, tag_y + 22, "Already installed", SS_C_GRAY);
    }

    /* Show everything above immediately — ensure_thumbnail() below is a
     * blocking network call that can take up to ~15-30s on a cold session
     * (fresh TLS handshake + GET). Drawing it before this point left the
     * screen showing the stale previous frame for that whole window with no
     * feedback at all, indistinguishable from a freeze. */
    akira_display_rect_outline(thumb_x, thumb_y, THUMB_W, THUMB_H, SS_C_GRAY);
    akira_display_text(thumb_x + 18, thumb_y + THUMB_H / 2 - 6, "Loading...", SS_C_GRAY);
    ss_draw_ribbon("[A] Install", "[B] Back");
    akira_display_flush();

    ensure_thumbnail(g_detail_idx);

    if (g_thumb_valid) {
        akira_display_bitmap(thumb_x, thumb_y, THUMB_W, THUMB_H, g_thumb_rgb);
    } else {
        akira_display_rect_outline(thumb_x, thumb_y, THUMB_W, THUMB_H, SS_C_GRAY);
        akira_display_text(thumb_x + 14, thumb_y + THUMB_H / 2 - 6, "No preview", SS_C_GRAY);
    }
    akira_display_flush();
}

static void draw_search(void)
{
    akira_display_clear(SS_C_BLACK);
    ss_draw_header("SEARCH APPS");
    akira_display_rect(0, SS_CONT_Y, SS_SCR_W, SS_RIB_Y - SS_CONT_Y, SS_C_BLACK);

    char q[40];
    snprintf(q, sizeof(q), "> %s_", g_search_query);
    akira_display_text(20, SS_CONT_Y + 8, q, SS_C_WHITE);

    const int cell_w = 34, cell_h = 24, pad = 4;
    int grid_x = (SS_SCR_W - KB_COLS * cell_w) / 2;
    int grid_y = SS_CONT_Y + 34;

    for (int r = 0; r < KB_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            char ch = KB_LAYOUT[r][c];
            if (ch == '_') {
                continue;
            }
            int cx = grid_x + c * cell_w;
            int cy = grid_y + r * cell_h;
            bool hi = (r == g_kb_row && c == g_kb_col);
            akira_ui_dither_card(cx, cy, cell_w - pad, cell_h - pad, 4, hi, 2);
            char s[2] = { ch, '\0' };
            akira_display_text(cx + (cell_w - pad) / 2 - 4, cy + (cell_h - pad) / 2 - 6,
                               s, hi ? SS_C_BLACK : SS_C_WHITE);
        }
    }

    ss_draw_ribbon("[A] Add  X Del", "[B] Search  Y Clr");
    akira_display_flush();
}

static void redraw(void)
{
    switch (g_state) {
    case PLAY_LOADING: draw_loading(); break;
    case PLAY_LIST:    draw_list();    break;
    case PLAY_DETAIL:  draw_detail();  break;
    case PLAY_SEARCH:  draw_search();  break;
    case PLAY_ERROR:   draw_error();   break;
    }
}

/* ------------------------------------------------------------------ */
/* Install                                                              */
/* ------------------------------------------------------------------ */
/* app_manager_install_akpkg() (gzip inflate + tar parse + PQC signature
 * verify) overflows the akira_os_shell thread's stack, which is sized for UI
 * rendering and file I/O, not decompression + crypto. Offload just that call
 * to the system workqueue, sized for exactly this kind of load
 * (CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=12280 — see sd_install_screen.c's
 * do_install_work for the established pattern).
 *
 * The download itself (catalog_https_get) stays on the calling thread —
 * NOT also offloaded to the system workqueue. That call's async connect
 * (net_stream_connect) depends on a k_work also dispatched via the system
 * workqueue; running the download itself as a work item on that same queue
 * makes it wait on its own connect, which can never run until it yields —
 * self-starvation (connect_work_fn only gets to run, and complete, after the
 * caller already gave up, surfacing as "connected" immediately followed by
 * "closed"). Safe to submit the install-only work below because by then
 * catalog_https_get() has already returned and closed its
 * stream — nothing of ours is left pending on the workqueue to contend with. */
static int g_install_entry_idx;
static size_t g_install_pkg_len;
static volatile bool g_install_done;
static int g_install_result;

static void do_akpkg_install_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    const catalogue_entry_t *e = &g_entries[g_install_entry_idx];

    char name_buf[APP_NAME_MAX_LEN];
    strncpy(name_buf, e->name, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';

    g_install_result = app_manager_install_akpkg(name_buf, sizeof(name_buf),
                                                 g_pkg_buf, g_install_pkg_len,
                                                 APP_SOURCE_HTTP);
    g_install_done = true;
}

K_WORK_DEFINE(g_akpkg_install_work, do_akpkg_install_work_fn);

static void do_install(const catalogue_entry_t *e)
{
    char q[48];
    snprintf(q, sizeof(q), "Install %s?", e->display_name);
    if (!akira_ui_confirm_dialog("APP_INSTALL", q)) {
        return;
    }

    char host[128], path[256];
    if (split_https_url(e->download_url, host, sizeof(host), path, sizeof(path)) < 0) {
        install_progress_show(e->display_name, 0, "Bad download URL");
        k_sleep(K_MSEC(1500));
        return;
    }
    (void)host; /* download_url is always same-origin as PLAY_HOST — only path is needed */

    /* Reuses the catalogue fetch's kept-alive session where possible —
     * see catalog_session_ensure(). If the session went stale (peer closed
     * an idle connection while the user browsed the list, or a request on
     * it failed), drop it and reopen fresh before the next attempt rather
     * than surface a single failed request as a hard error. */
    int n = -ETIMEDOUT;
    for (int attempt = 1; attempt <= 3; attempt++) {
        char msg[24];
        snprintf(msg, sizeof(msg), "Downloading... (%d/3)", attempt);
        install_progress_show(e->display_name, 10, msg);

        int handle = catalog_session_ensure();
        n = (handle >= 0)
            ? catalog_https_get_on(handle, PLAY_HOST, path, g_pkg_buf,
                                   sizeof(g_pkg_buf), PLAY_DOWNLOAD_TIMEOUT_MS)
            : handle;
        if (n >= 0) {
            break;
        }
        LOG_WRN("Download attempt %d failed: %d", attempt, n);
        catalog_session_close();
    }
    if (n < 0) {
        LOG_ERR("Download failed after 3 attempts: %d", n);
        install_progress_show(e->display_name, 10, "Download failed");
        k_sleep(K_MSEC(1500));
        return;
    }

    install_progress_show(e->display_name, 70, "Installing...");
    g_install_entry_idx = g_detail_idx;
    g_install_pkg_len = (size_t)n;
    g_install_done = false;
    k_work_submit(&g_akpkg_install_work);
    while (!g_install_done) {
        k_sleep(K_MSEC(50));
    }

    if (g_install_result < 0) {
        LOG_ERR("Install failed: %d", g_install_result);
        install_progress_show(e->display_name, 70, "Install failed");
        k_sleep(K_MSEC(1500));
        return;
    }

    install_progress_show(e->display_name, 100, "Installed!");
    k_sleep(K_MSEC(1000));
}

/* ------------------------------------------------------------------ */
/* Key handlers                                                        */
/* ------------------------------------------------------------------ */
static void handle_list(uint32_t just)
{
    int vis = ss_menu_vis_count(SS_CONT_Y + 16);

    if (just & BIT(AKIRA_BTN_UP)) {
        if (g_sel > 0) {
            g_sel--;
            g_scroll = ss_scroll_clamp(g_sel, g_scroll, g_filtered_count, SS_CONT_Y + 16);
            redraw();
        }
    }
    if (just & BIT(AKIRA_BTN_DOWN)) {
        if (g_sel < g_filtered_count - 1) {
            g_sel++;
            g_scroll = ss_scroll_clamp(g_sel, g_scroll, g_filtered_count, SS_CONT_Y + 16);
            redraw();
        }
    }
    if (just & (BIT(AKIRA_BTN_LEFT) | BIT(AKIRA_BTN_RIGHT))) {
        printk("AKPLAY: tab switch start, g_tab=%d filtered=%d sel=%d scroll=%d\n",
               g_tab, g_filtered_count, g_sel, g_scroll);
        g_tab = (just & BIT(AKIRA_BTN_LEFT))
            ? (g_tab - 1 + TAB_COUNT) % TAB_COUNT
            : (g_tab + 1) % TAB_COUNT;
        printk("AKPLAY: new g_tab=%d, calling rebuild_filter\n", g_tab);
        rebuild_filter();
        printk("AKPLAY: rebuild_filter done, filtered=%d, calling redraw\n", g_filtered_count);
        redraw();
        printk("AKPLAY: redraw done\n");
    }
    if (just & BIT(AKIRA_BTN_X)) {
        g_tag_filter++;
        if (g_tag_filter >= CATALOGUE_TAG_COUNT) {
            g_tag_filter = -1;
        }
        rebuild_filter();
        redraw();
    }
    if (just & BIT(AKIRA_BTN_Y)) {
        g_kb_row = 0;
        g_kb_col = 0;
        g_state = PLAY_SEARCH;
        redraw();
    }
    if ((just & BIT(AKIRA_BTN_A)) && g_filtered_count > 0) {
        g_detail_idx = g_filtered[g_sel];
        g_state = PLAY_DETAIL;
        redraw();
    }
    if ((just & BIT(AKIRA_BTN_B)) || (just & BIT(AKIRA_BTN_HOME))) {
        g_active = false;
        home_screen_load();
    }

    (void)vis;
}

static void handle_detail(uint32_t just)
{
    if (just & BIT(AKIRA_BTN_A)) {
        do_install(&g_entries[g_detail_idx]);
        g_state = PLAY_LIST;
        redraw();
    }
    if (just & BIT(AKIRA_BTN_B)) {
        g_state = PLAY_LIST;
        redraw();
    }
}

static void handle_search(uint32_t just)
{
    if (just & BIT(AKIRA_BTN_UP) && g_kb_row > 0) {
        g_kb_row--;
        redraw();
    }
    if (just & BIT(AKIRA_BTN_DOWN) && g_kb_row < KB_ROWS - 1) {
        g_kb_row++;
        redraw();
    }
    if (just & BIT(AKIRA_BTN_LEFT) && g_kb_col > 0) {
        g_kb_col--;
        redraw();
    }
    if (just & BIT(AKIRA_BTN_RIGHT) && g_kb_col < KB_COLS - 1) {
        g_kb_col++;
        redraw();
    }
    if (just & BIT(AKIRA_BTN_A)) {
        char c = KB_LAYOUT[g_kb_row][g_kb_col];
        size_t len = strlen(g_search_query);
        if (c != '_' && len + 1 < sizeof(g_search_query)) {
            g_search_query[len] = c;
            g_search_query[len + 1] = '\0';
        }
        redraw();
    }
    if (just & BIT(AKIRA_BTN_X)) {
        size_t len = strlen(g_search_query);
        if (len > 0) {
            g_search_query[len - 1] = '\0';
        }
        redraw();
    }
    if (just & BIT(AKIRA_BTN_Y)) {
        g_search_query[0] = '\0';
        redraw();
    }
    if (just & BIT(AKIRA_BTN_B)) {
        rebuild_filter();
        g_state = PLAY_LIST;
        redraw();
    }
    if (just & BIT(AKIRA_BTN_HOME)) {
        g_active = false;
        home_screen_load();
    }
}

static void handle_error(uint32_t just)
{
    if (just & BIT(AKIRA_BTN_A)) {
        g_state = PLAY_LOADING;
        redraw();
        int ret = fetch_catalogue();
        if (ret < 0) {
            set_error_from_errno(ret);
            g_state = PLAY_ERROR;
        } else {
            g_tab = 0;
            rebuild_filter();
            g_state = PLAY_LIST;
        }
        redraw();
    }
    if (just & (BIT(AKIRA_BTN_B) | BIT(AKIRA_BTN_HOME))) {
        g_active = false;
        home_screen_load();
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void akiraplay_screen_load(void)
{
    g_active = true;
    g_state = PLAY_LOADING;
    g_tag_filter = -1;
    g_search_query[0] = '\0';
    redraw();

    int ret = fetch_catalogue();
    if (ret < 0) {
        set_error_from_errno(ret);
        g_state = PLAY_ERROR;
    } else {
        g_tab = 0;
        rebuild_filter();
        g_state = PLAY_LIST;
    }
    redraw();

    uint32_t prev = akira_input_get_bitmask();
    while (g_active) {
        k_sleep(K_MSEC(20));
        uint32_t btns = akira_input_get_bitmask();
        uint32_t just = btns & ~prev;
        prev = btns;
        if (!just) {
            continue;
        }

        switch (g_state) {
        case PLAY_LIST:   handle_list(just);   break;
        case PLAY_DETAIL: handle_detail(just); break;
        case PLAY_SEARCH: handle_search(just); break;
        case PLAY_ERROR:  handle_error(just);  break;
        default: break;
        }
        prev = akira_input_get_bitmask(); /* swallow buttons held from any blocking dialog/install */
    }

    catalog_session_close();
}
