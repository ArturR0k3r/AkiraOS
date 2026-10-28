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
 *        WiFi, as one screen on the shell's screen stack (ui/screen.h).
 *
 * Flow: LOADING (fetch + parse catalogue.json) -> LIST (LEFT/RIGHT cycle the
 * category tab, X cycles a tag filter, Y opens on-screen search) -> DETAIL
 * (thumbnail + tags, A to install) -> back to LIST. SEARCH is a D-pad letter
 * grid that applies a substring filter on display_name. ERROR on fetch
 * failure (no WiFi, timeout, bad JSON), with retry. INSTALL is DETAIL with
 * the keys locked while the package downloads and installs; progress and the
 * outcome go to the overlay progress card.
 *
 * Network: there is no thread to spare for TLS (internal RAM is full, thread
 * stacks can't live in PSRAM, and the system workqueue's stack is too small
 * for a handshake), so every request still runs synchronously on the shell
 * thread — but only from tick(), wrapped in screen_run_busy(), and only once
 * the frame announcing it has been painted (see schedule_job()). draw()
 * never touches the network.
 */

#include "akiraplay_screen.h"
#include "home_screen.h"
#include "shell_theme.h"
#include "catalog_net.h"
#include "catalogue_parser.h"
#include "ui/akira_ui.h"
#include "ui/anim.h"
#include "ui/overlay.h"
#include "ui/screen.h"

#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <runtime/app_manager/app_manager.h>
#include <connectivity/wifi/wifi_manager.h>

#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#define PLAY_HOST "console.app.akiraos.dev"
#define PLAY_CATALOGUE_PATH "/catalog/catalogue.json"
#define PLAY_JSON_BUF_SIZE 16384
#define PLAY_PKG_BUF_SIZE (256 * 1024)
#define PLAY_FETCH_TIMEOUT_MS 15000
#define PLAY_DOWNLOAD_TIMEOUT_MS 30000
#define PLAY_DL_ATTEMPTS 3

/* On-device thumbnail contract, matches AkiraConsoleApp's submit.ts/
 * SubmitApp.tsx: fixed 96x64 1bpp bitmap, MSB-first, rows byte-aligned
 * (96/8=12 B/row exactly, no padding). Kept 1-bit to match this UI kit's
 * dither-only palette and to keep the download tiny (768 B vs a full-color
 * image) — decoded to RGB565 once per fetch, into g_thumb_rgb, for the blit. */
#define THUMB_W 96
#define THUMB_H 64
#define THUMB_ROW_BYTES (THUMB_W / 8)
#define THUMB_PACKED_BYTES (THUMB_ROW_BYTES * THUMB_H)

/* List geometry — same rhythm as the settings lists (ui/list_screen.c). */
#define ROW_H 34
#define ROW_TOP (SCREEN_CONTENT_Y + 2)
#define ROW_X 12
#define ROW_W (SCR_W - 30) /* leaves a lane for the scrollbar */
#define CARD_H (ROW_H - 6)
#define VISIBLE_ROWS ((SCREEN_CONTENT_B - ROW_TOP) / ROW_H)
#define BAND_MID ((SCREEN_CONTENT_Y + SCREEN_CONTENT_B) / 2)
#define SEL_MS 130

/* Detail page: framed thumbnail left, facts right, tag badges underneath. */
#define THUMB_X 16
#define THUMB_Y (SCREEN_CONTENT_Y + 10)
#define INFO_X (THUMB_X + THUMB_W + 14)

static uint8_t g_json_buf[PLAY_JSON_BUF_SIZE] __attribute__((section(".ext_ram.bss")));
static uint8_t g_pkg_buf[PLAY_PKG_BUF_SIZE] __attribute__((section(".ext_ram.bss")));
static catalogue_entry_t g_entries[CATALOGUE_MAX_APPS] __attribute__((section(".ext_ram.bss")));
static uint8_t g_thumb_packed[THUMB_PACKED_BYTES] __attribute__((section(".ext_ram.bss")));
static uint16_t g_thumb_rgb[THUMB_W * THUMB_H] __attribute__((section(".ext_ram.bss")));

/* URL scratch — static rather than local: the requests run several call
 * frames deep (shell loop -> screen_frame -> tick -> screen_run_busy -> job)
 * above the stack-hungry TLS send/close code, on the 8192-byte shell thread
 * stack (SHELL_THREAD_STACK_SIZE) — every on-stack byte here narrows an
 * already-tight budget. The download path must also outlive one tick, as
 * each retry runs from its own tick. */
static char g_url_host[128] __attribute__((section(".ext_ram.bss")));
static char g_thumb_url_path[256] __attribute__((section(".ext_ram.bss")));
static char g_dl_url_path[256] __attribute__((section(".ext_ram.bss")));

/* Status-bar title for the list (tab / tag / query) — must outlive the
 * screen, see screen_set_title(). */
static char g_title[64] __attribute__((section(".ext_ram.bss")));

typedef enum {
    PLAY_LOADING = 0,
    PLAY_LIST,
    PLAY_DETAIL,
    PLAY_SEARCH,
    PLAY_ERROR,
    PLAY_INSTALL, /* DETAIL with keys locked: download + install running */
} play_state_t;

/* A network request queued by a key press (or enter), run from tick(). */
typedef enum {
    JOB_NONE = 0,
    JOB_CATALOGUE,
    JOB_THUMB,
    JOB_DOWNLOAD,
} play_job_t;

static play_state_t g_state;
static play_job_t g_job;
static bool g_job_shown; /* the frame announcing g_job has been painted */

static int g_count;                       /* total entries parsed */
static int g_filtered[CATALOGUE_MAX_APPS]; /* indices into g_entries */
static int g_filtered_count;
static int g_sel, g_scroll; /* g_scroll = first fully visible row */
static anim_t g_hl;         /* highlight position, px from the list's origin */
static anim_t g_scroll_px;  /* scroll position, px */
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

static void set_state(play_state_t s);

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
    anim_set(&g_hl, 0);
    anim_set(&g_scroll_px, 0);
}

/* "PLAY: All", "PLAY:Retro|NES", "PLAY:All|"ZEL"", ... */
static void update_list_title(void)
{
    if (g_tag_filter >= 0 && g_search_query[0]) {
        snprintf(g_title, sizeof(g_title), "PLAY:%s|%s|\"%s\"",
                 TAB_LABELS[g_tab], catalogue_tag_name(g_tag_filter), g_search_query);
    } else if (g_tag_filter >= 0) {
        snprintf(g_title, sizeof(g_title), "PLAY:%s|%s", TAB_LABELS[g_tab], catalogue_tag_name(g_tag_filter));
    } else if (g_search_query[0]) {
        snprintf(g_title, sizeof(g_title), "PLAY:%s|\"%s\"", TAB_LABELS[g_tab], g_search_query);
    } else {
        snprintf(g_title, sizeof(g_title), "PLAY: %s", TAB_LABELS[g_tab]);
    }
    screen_set_title(g_title);
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
 * stale (peer closed an idle connection while the user was browsing), and
 * closed in leave() — including when HOME long-press pops us. */
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

    int parsed = catalogue_parse((const char *)g_json_buf, (size_t)n,
                                 g_entries, CATALOGUE_MAX_APPS);
    if (parsed < 0) {
        g_count = 0;
        return parsed;
    }
    g_count = parsed;
    g_thumb_idx = -1; /* g_entries was just overwritten, drop the stale cache */
    g_thumb_valid = false;
    return 0;
}

/* Fetches and decodes the thumbnail for g_entries[idx] on first access, then
 * caches by index — opening the same detail page again costs nothing, and
 * the network round trip (on the already-open keep-alive session) only
 * happens once per catalogue load per app, failed or not. Runs from tick()
 * (JOB_THUMB) under screen_run_busy(), never from draw(). */
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

    if (split_https_url(e->thumbnail_url, g_url_host, sizeof(g_url_host),
                        g_thumb_url_path, sizeof(g_thumb_url_path)) < 0) {
        return;
    }
    /* g_url_host unused past this point — thumbnail_url is always
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
            /* bit=1 is PAPER (dark/foreground) — C_WHITE is that role's
             * RGB565 value despite the name (see shell_theme.h). */
            g_thumb_rgb[y * THUMB_W + x] = bit ? C_WHITE : C_BLACK;
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

/* ---- screen_run_busy() bodies --------------------------------------- */
static void catalogue_job(void *arg)
{
    *(int *)arg = fetch_catalogue();
}

static void thumb_job(void *arg)
{
    ARG_UNUSED(arg);
    ensure_thumbnail(g_detail_idx);
}

static void download_job(void *arg)
{
    int handle = catalog_session_ensure();
    *(int *)arg = (handle >= 0)
        ? catalog_https_get_on(handle, PLAY_HOST, g_dl_url_path, g_pkg_buf,
                               sizeof(g_pkg_buf), PLAY_DOWNLOAD_TIMEOUT_MS)
        : handle;
}

/* Queue @p job for tick(). It only runs once the frame that announces it
 * (loading text, "Loading..." thumbnail slot, progress card) has been
 * painted, so the user sees why the screen is about to pause. */
static void schedule_job(play_job_t job)
{
    g_job = job;
    g_job_shown = false;
    screen_invalidate();
}

/* ------------------------------------------------------------------ */
/* Draw (content band only — the framework owns status bar + footer)   */
/* ------------------------------------------------------------------ */
static void draw_centred(int y, const char *s, uint16_t fg)
{
    akira_display_text((SCR_W - akira_ui_text_w(s, AKIRA_UI_FONT_SMALL)) / 2, y, s, fg);
}

/* Text cut to @p max_w px with a ".." tail. */
static void draw_clipped(int x, int y, int max_w, const char *s, uint16_t fg)
{
    char buf[48];
    int max_chars = MAX(max_w / AKIRA_UI_CELL_W, 3);

    if ((int)strlen(s) > max_chars) {
        snprintf(buf, sizeof(buf), "%.*s..", MIN(max_chars - 2, (int)sizeof(buf) - 3), s);
        s = buf;
    }
    akira_display_text(x, y, s, fg);
}

static void draw_loading(void)
{
    draw_centred(BAND_MID - 5, "Loading catalogue...", C_WHITE);
}

static void draw_error(void)
{
    int w = SCR_W - 48, h = 64;
    int x = (SCR_W - w) / 2, y = BAND_MID - h / 2;

    akira_ui_dither_card(x, y, w, h, 10, false, 3);
    draw_centred(y + 16, "Couldn't load the store", C_GRAY);
    draw_centred(y + 36, g_error_msg, C_WHITE);
}

static void draw_list_row(const catalogue_entry_t *e, int y, bool inverted)
{
    uint16_t fg = inverted ? C_BLACK : C_WHITE;
    int ty = y + (CARD_H - 10) / 2;
    int right = ROW_X + ROW_W - 10;

    if (e->size_bytes >= 1024) {
        char sz[16];
        snprintf(sz, sizeof(sz), "%uKB", (unsigned)((e->size_bytes + 1023) / 1024));
        right -= akira_ui_text_w(sz, AKIRA_UI_FONT_SMALL);
        akira_display_text(right, ty, sz, fg);
        right -= 8;
    }
    draw_clipped(ROW_X + 10, ty, right - (ROW_X + 10), e->display_name, fg);
}

static bool row_off_band(int y)
{
    return y + CARD_H < SCREEN_CONTENT_Y - 4 || y > SCREEN_CONTENT_B;
}

static void draw_list(void)
{
    if (g_filtered_count == 0) {
        draw_centred(BAND_MID - 5, "No apps in this category", C_GRAY);
        return;
    }

    int scroll = anim_value(&g_scroll_px);
    int hl = anim_value(&g_hl);
    int first = MAX(scroll / ROW_H - 1, 0);
    int last = MIN(first + VISIBLE_ROWS + 2, g_filtered_count);

    /* Idle cards first, then the sliding highlight over them, then labels. */
    for (int i = first; i < last; i++) {
        int y = ROW_TOP + i * ROW_H - scroll;
        if (!row_off_band(y)) {
            akira_ui_dither_card(ROW_X, y, ROW_W, CARD_H, 8, false, 2);
        }
    }
    akira_ui_dither_card(ROW_X, ROW_TOP + hl - scroll, ROW_W, CARD_H, 8, true, 3);
    for (int i = first; i < last; i++) {
        int y = ROW_TOP + i * ROW_H - scroll;
        if (!row_off_band(y)) {
            draw_list_row(&g_entries[g_filtered[i]], y, abs(hl - i * ROW_H) < ROW_H / 2);
        }
    }

    /* Scrollbar lane — only when the list overflows */
    if (g_filtered_count > VISIBLE_ROWS) {
        int track_y = ROW_TOP, track_h = VISIBLE_ROWS * ROW_H - 6;
        int max_scroll = (g_filtered_count - VISIBLE_ROWS) * ROW_H;
        int knob_h = MAX(track_h * VISIBLE_ROWS / g_filtered_count, 12);
        int knob_y = track_y + (track_h - knob_h) * CLAMP(scroll, 0, max_scroll) / max_scroll;

        akira_display_vline(SCR_W - 9, track_y, track_h, C_DKGRAY);
        akira_display_rounded_rect_fill(SCR_W - 11, knob_y, 5, knob_h, 2, C_WHITE);
    }
}

static void draw_detail(void)
{
    const catalogue_entry_t *e = &g_entries[g_detail_idx];
    int info_w = SCR_W - INFO_X - 10;
    char line[48];

    /* Thumbnail in a frame card. Fetched by tick() (JOB_THUMB), not here:
     * until then the slot says "Loading...". */
    akira_ui_dither_card(THUMB_X - 4, THUMB_Y - 4, THUMB_W + 8, THUMB_H + 8, 6, false, 3);
    if (g_thumb_idx == g_detail_idx && g_thumb_valid) {
        akira_display_bitmap(THUMB_X, THUMB_Y, THUMB_W, THUMB_H, g_thumb_rgb);
    } else {
        const char *ph = (g_thumb_idx == g_detail_idx) ? "No preview" : "Loading...";
        akira_display_text(THUMB_X + (THUMB_W - akira_ui_text_w(ph, AKIRA_UI_FONT_SMALL)) / 2,
                           THUMB_Y + THUMB_H / 2 - 5, ph, C_GRAY);
    }

    int y = THUMB_Y + 2;
    draw_clipped(INFO_X, y, info_w, e->display_name, C_WHITE);
    snprintf(line, sizeof(line), "Version: %s", e->version);
    draw_clipped(INFO_X, y + 20, info_w, line, C_WHITE);
    snprintf(line, sizeof(line), "Category: %s", e->category);
    draw_clipped(INFO_X, y + 36, info_w, line, C_WHITE);
    if (e->size_bytes >= 1024) {
        snprintf(line, sizeof(line), "Size: %uKB", (unsigned)((e->size_bytes + 1023) / 1024));
    } else {
        snprintf(line, sizeof(line), "Size: %u B", (unsigned)e->size_bytes);
    }
    draw_clipped(INFO_X, y + 52, info_w, line, C_WHITE);

    /* Tag badges, wrapping to a second row; capped at 6 to keep a bounded
     * worst-case height above the footer (and the progress card). */
    int tag_x = THUMB_X - 4, tag_y = THUMB_Y + THUMB_H + 14;
    int shown = 0;
    for (int i = 0; i < CATALOGUE_TAG_COUNT && shown < 6; i++) {
        if (!(e->tag_mask & (1u << i))) {
            continue;
        }
        const char *name = catalogue_tag_name(i);
        int w = akira_ui_text_w(name, AKIRA_UI_FONT_SMALL) + 10;
        if (tag_x + w > SCR_W - 10) {
            tag_x = THUMB_X - 4;
            tag_y += 20;
        }
        akira_ui_tag(tag_x, tag_y, name, false);
        tag_x += w + 4;
        shown++;
    }

    if (app_manager_get_state(e->name) != APP_STATE_NEW) {
        akira_display_text(THUMB_X - 4, shown ? tag_y + 24 : tag_y, "Already installed", C_GRAY);
    }
}

static void draw_search(void)
{
    const int cell_w = 34, cell_h = 24, pad = 4;
    int grid_x = (SCR_W - KB_COLS * cell_w) / 2;
    int grid_y = SCREEN_CONTENT_Y + 44;
    int box_y = SCREEN_CONTENT_Y + 6, box_w = KB_COLS * cell_w - pad;

    /* Query field — shows the tail when the query outgrows the box. */
    char q[CATALOGUE_NAME_LEN + 4];
    int max_chars = (box_w - 20) / AKIRA_UI_CELL_W - 3; /* room for "> " and "_" */
    size_t len = strlen(g_search_query);
    const char *tail = g_search_query + ((int)len > max_chars ? len - (size_t)max_chars : 0);

    akira_ui_dither_card(grid_x, box_y, box_w, 26, 8, false, 2);
    snprintf(q, sizeof(q), "> %s_", tail);
    akira_display_text(grid_x + 10, box_y + 8, q, C_WHITE);

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
                               s, hi ? C_BLACK : C_WHITE);
        }
    }
}

static void play_draw(void)
{
    switch (g_state) {
    case PLAY_LOADING: draw_loading(); break;
    case PLAY_LIST:    draw_list();    break;
    case PLAY_DETAIL:
    case PLAY_INSTALL: draw_detail();  break;
    case PLAY_SEARCH:  draw_search();  break;
    case PLAY_ERROR:   draw_error();   break;
    }
    g_job_shown = true; /* tick() may now run what this frame announced */
}

/* ------------------------------------------------------------------ */
/* Install                                                              */
/* ------------------------------------------------------------------ */
/* app_manager_install_akpkg() (gzip inflate + tar parse + PQC signature
 * verify) overflows the akira_os_shell thread's stack, which is sized for UI
 * rendering and file I/O, not decompression + crypto. Offload just that call
 * to the system workqueue, sized for exactly this kind of load
 * (CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=12280 — see sd_install_screen.c's
 * do_install_work for the established pattern). tick() polls for the
 * outcome (install_report()) instead of the shell thread waiting on it.
 *
 * The download itself (catalog_https_get_on) stays on the shell thread —
 * NOT also offloaded to the system workqueue. That call's async connect
 * (net_stream_connect) depends on a k_work also dispatched via the system
 * workqueue; running the download itself as a work item on that same queue
 * makes it wait on its own connect, which can never run until it yields —
 * self-starvation (connect_work_fn only gets to run, and complete, after the
 * caller already gave up, surfacing as "connected" immediately followed by
 * "closed"). Safe to submit the install-only work below because by then
 * catalog_https_get_on() has already returned — nothing of ours is left
 * pending on the workqueue to contend with. */
static char g_install_name[APP_NAME_MAX_LEN] __attribute__((section(".ext_ram.bss")));
static size_t g_install_pkg_len;
static volatile bool g_install_done;
static volatile int g_install_result;
static bool g_install_inflight; /* submitted, outcome not reported yet */
static int g_dl_attempt;        /* 1..PLAY_DL_ATTEMPTS */

static void do_akpkg_install_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    char name_buf[APP_NAME_MAX_LEN];
    strncpy(name_buf, g_install_name, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';

    g_install_result = app_manager_install_akpkg(name_buf, sizeof(name_buf),
                                                 g_pkg_buf, g_install_pkg_len,
                                                 APP_SOURCE_HTTP);
    g_install_done = true;
}

K_WORK_DEFINE(g_akpkg_install_work, do_akpkg_install_work_fn);

static void progress_downloading(void)
{
    char msg[32];
    snprintf(msg, sizeof(msg), "Downloading... (%d/%d)", g_dl_attempt, PLAY_DL_ATTEMPTS);
    overlay_progress(g_entries[g_detail_idx].display_name, 10, msg);
}

/* Report a finished install job on the progress card.
 * @return true if an outcome was reported. Never touches the screen's
 * title/hints, so it is safe from leave() too. */
static bool install_report(void)
{
    if (!g_install_inflight || !g_install_done) {
        return false;
    }
    g_install_inflight = false;

    if (g_install_result < 0) {
        LOG_ERR("Install failed: %d", g_install_result);
        overlay_progress_finish(false, "Install failed");
    } else {
        overlay_progress_finish(true, "Installed!");
        home_screen_refresh();
    }
    return true;
}

static void install_start(size_t pkg_len)
{
    overlay_progress(NULL, 70, "Installing...");
    g_install_pkg_len = pkg_len;
    g_install_done = false;
    g_install_inflight = true;
    k_work_submit(&g_akpkg_install_work);
}

/* One download attempt per tick, so the progress card shows the attempt
 * count between tries. Reuses the catalogue fetch's kept-alive session where
 * possible — see catalog_session_ensure(). If the session went stale (peer
 * closed an idle connection while the user browsed the list, or a request on
 * it failed), drop it and reopen fresh before the next attempt rather than
 * surface a single failed request as a hard error. */
static void download_step(void)
{
    char label[24];
    int n = -ETIMEDOUT;

    snprintf(label, sizeof(label), "Downloading (%d/%d)", g_dl_attempt, PLAY_DL_ATTEMPTS);
    screen_run_busy(label, download_job, &n);
    if (n >= 0) {
        install_start((size_t)n);
        return;
    }

    LOG_WRN("Download attempt %d failed: %d", g_dl_attempt, n);
    catalog_session_close();
    if (g_dl_attempt < PLAY_DL_ATTEMPTS) {
        g_dl_attempt++;
        progress_downloading();
        schedule_job(JOB_DOWNLOAD);
        return;
    }

    LOG_ERR("Download failed after %d attempts: %d", PLAY_DL_ATTEMPTS, n);
    overlay_progress_finish(false, "Download failed");
    set_state(PLAY_LIST);
}

/* APP_INSTALL confirm answered — the dialog has already been popped. */
static void install_confirmed(bool confirmed, void *ctx)
{
    ARG_UNUSED(ctx);
    const catalogue_entry_t *e = &g_entries[g_detail_idx];

    if (!confirmed) {
        set_state(PLAY_LIST);
        return;
    }

    if (split_https_url(e->download_url, g_url_host, sizeof(g_url_host),
                        g_dl_url_path, sizeof(g_dl_url_path)) < 0) {
        overlay_progress(e->display_name, 0, NULL);
        overlay_progress_finish(false, "Bad download URL");
        set_state(PLAY_LIST);
        return;
    }
    /* download_url is always same-origin as PLAY_HOST — only path is needed */

    snprintf(g_install_name, sizeof(g_install_name), "%s", e->name);
    g_dl_attempt = 1;
    progress_downloading();
    set_state(PLAY_INSTALL);
    schedule_job(JOB_DOWNLOAD);
}

/* ------------------------------------------------------------------ */
/* State + key handlers                                                */
/* ------------------------------------------------------------------ */
static void set_state(play_state_t s)
{
    g_state = s;
    if (g_job == JOB_THUMB && s != PLAY_DETAIL) {
        g_job = JOB_NONE; /* left the detail page before its preview loaded */
    }

    switch (s) {
    case PLAY_LOADING:
        screen_set_title("AKIRA PLAY");
        screen_set_hints(NULL, "B:Back");
        break;
    case PLAY_ERROR:
        screen_set_title("AKIRA PLAY");
        screen_set_hints("A:Retry", "B:Back");
        break;
    case PLAY_LIST:
        update_list_title();
        screen_set_hints("A:View|<>:Cat|X:Tag|Y:Find", "B:Back");
        break;
    case PLAY_DETAIL:
        screen_set_title("APP DETAIL");
        screen_set_hints("A:Install", "B:Back");
        break;
    case PLAY_SEARCH:
        screen_set_title("SEARCH APPS");
        screen_set_hints("A:Add|X:Del|Y:Clear", "B:Search");
        break;
    case PLAY_INSTALL:
        screen_set_title("APP DETAIL");
        screen_set_hints(NULL, NULL); /* keys locked until the outcome */
        break;
    }
    screen_invalidate();
}

static void move_sel(int to)
{
    if (g_filtered_count == 0) {
        return;
    }
    to = CLAMP(to, 0, g_filtered_count - 1);
    if (to == g_sel) {
        return;
    }
    g_sel = to;
    anim_to(&g_hl, to * ROW_H, SEL_MS, ANIM_EASE_OUT);

    int scroll = g_scroll;
    if (to < scroll) {
        scroll = to;
    } else if (to >= scroll + VISIBLE_ROWS) {
        scroll = to - VISIBLE_ROWS + 1;
    }
    if (scroll != g_scroll) {
        g_scroll = scroll;
        anim_to(&g_scroll_px, scroll * ROW_H, SEL_MS + 40, ANIM_EASE_OUT);
    }
    screen_invalidate();
}

static void open_detail(int idx)
{
    g_detail_idx = idx;
    set_state(PLAY_DETAIL);
    if (g_thumb_idx != idx) {
        if (g_entries[idx].thumbnail_url[0] == '\0') {
            ensure_thumbnail(idx); /* nothing to fetch — just caches the miss */
        } else {
            schedule_job(JOB_THUMB);
        }
    }
}

static void handle_list(uint32_t just)
{
    if (just & BIT(AKIRA_BTN_B)) {
        screen_pop();
        return;
    }
    if (just & BIT(AKIRA_BTN_UP)) {
        move_sel(g_sel - 1);
    }
    if (just & BIT(AKIRA_BTN_DOWN)) {
        move_sel(g_sel + 1);
    }
    if (just & (BIT(AKIRA_BTN_LEFT) | BIT(AKIRA_BTN_RIGHT))) {
        g_tab = (just & BIT(AKIRA_BTN_LEFT))
            ? (g_tab - 1 + TAB_COUNT) % TAB_COUNT
            : (g_tab + 1) % TAB_COUNT;
        rebuild_filter();
        update_list_title();
        screen_invalidate();
    }
    if (just & BIT(AKIRA_BTN_X)) {
        g_tag_filter++;
        if (g_tag_filter >= CATALOGUE_TAG_COUNT) {
            g_tag_filter = -1;
        }
        rebuild_filter();
        update_list_title();
        screen_invalidate();
    }
    if (just & BIT(AKIRA_BTN_Y)) {
        g_kb_row = 0;
        g_kb_col = 0;
        set_state(PLAY_SEARCH);
        return;
    }
    if ((just & BIT(AKIRA_BTN_A)) && g_filtered_count > 0) {
        open_detail(g_filtered[g_sel]);
    }
}

static void handle_detail(uint32_t just)
{
    if (just & BIT(AKIRA_BTN_B)) {
        set_state(PLAY_LIST);
        return;
    }
    if (just & BIT(AKIRA_BTN_A)) {
        if (g_install_inflight) {
            /* g_pkg_buf still feeds the previous install's work item. */
            overlay_toast(TOAST_ICON_INFO, "Install in progress", "Try again when it's done", 2000);
            return;
        }
        char q[64]; /* copied by the dialog */
        snprintf(q, sizeof(q), "Install %s?", g_entries[g_detail_idx].display_name);
        akira_ui_confirm_open("APP_INSTALL", q, NULL, install_confirmed, NULL);
    }
}

/* Move the search cursor, never parking it on an unused (invisible) cell. */
static void kb_move(int drow, int dcol)
{
    int r = CLAMP(g_kb_row + drow, 0, KB_ROWS - 1);
    int c = CLAMP(g_kb_col + dcol, 0, KB_COLS - 1);

    while (c > 0 && KB_LAYOUT[r][c] == '_') {
        c--;
    }
    if (r != g_kb_row || c != g_kb_col) {
        g_kb_row = r;
        g_kb_col = c;
        screen_invalidate();
    }
}

static void handle_search(uint32_t just)
{
    if (just & BIT(AKIRA_BTN_UP)) {
        kb_move(-1, 0);
    }
    if (just & BIT(AKIRA_BTN_DOWN)) {
        kb_move(1, 0);
    }
    if (just & BIT(AKIRA_BTN_LEFT)) {
        kb_move(0, -1);
    }
    if (just & BIT(AKIRA_BTN_RIGHT)) {
        kb_move(0, 1);
    }
    if (just & BIT(AKIRA_BTN_A)) {
        char c = KB_LAYOUT[g_kb_row][g_kb_col];
        size_t len = strlen(g_search_query);
        if (c != '_' && len + 1 < sizeof(g_search_query)) {
            g_search_query[len] = c;
            g_search_query[len + 1] = '\0';
        }
        screen_invalidate();
    }
    if (just & BIT(AKIRA_BTN_X)) {
        size_t len = strlen(g_search_query);
        if (len > 0) {
            g_search_query[len - 1] = '\0';
        }
        screen_invalidate();
    }
    if (just & BIT(AKIRA_BTN_Y)) {
        g_search_query[0] = '\0';
        screen_invalidate();
    }
    if (just & BIT(AKIRA_BTN_B)) {
        rebuild_filter(); /* B applies the query and goes back to the list */
        set_state(PLAY_LIST);
    }
}

static void handle_error(uint32_t just)
{
    if (just & BIT(AKIRA_BTN_A)) {
        set_state(PLAY_LOADING);
        schedule_job(JOB_CATALOGUE);
        return;
    }
    if (just & BIT(AKIRA_BTN_B)) {
        screen_pop();
    }
}

/* ------------------------------------------------------------------ */
/* Screen callbacks                                                    */
/* ------------------------------------------------------------------ */
/* A short HOME edge is ignored everywhere: HOME long-press is the shell's,
 * and it pops us — leave() then cleans up. */
static void play_key(uint32_t just)
{
    switch (g_state) {
    case PLAY_LOADING:
        if (just & BIT(AKIRA_BTN_B)) {
            screen_pop();
        }
        break;
    case PLAY_LIST:   handle_list(just);   break;
    case PLAY_DETAIL: handle_detail(just); break;
    case PLAY_SEARCH: handle_search(just); break;
    case PLAY_ERROR:  handle_error(just);  break;
    case PLAY_INSTALL: break; /* locked until install_report() */
    }
}

static void run_job(play_job_t job)
{
    int ret = 0;

    switch (job) {
    case JOB_CATALOGUE:
        screen_run_busy("Loading catalogue", catalogue_job, &ret);
        if (ret < 0) {
            set_error_from_errno(ret);
            set_state(PLAY_ERROR);
        } else {
            g_tab = 0;
            rebuild_filter();
            set_state(PLAY_LIST);
        }
        break;
    case JOB_THUMB:
        screen_run_busy("Loading preview", thumb_job, NULL);
        break;
    case JOB_DOWNLOAD:
        download_step();
        break;
    default:
        break;
    }
}

static bool play_tick(void)
{
    bool redraw = false;

    if (install_report()) {
        if (g_state == PLAY_INSTALL) {
            set_state(PLAY_LIST);
        }
        redraw = true;
    }
    if (g_job != JOB_NONE && g_job_shown) {
        play_job_t job = g_job;

        g_job = JOB_NONE;
        run_job(job);
        redraw = true;
    }
    return redraw || anim_running(&g_hl) || anim_running(&g_scroll_px);
}

static void play_enter(void *arg)
{
    ARG_UNUSED(arg);
    g_tab = 0;
    g_tag_filter = -1;
    g_search_query[0] = '\0';
    g_count = 0;
    g_filtered_count = 0;
    g_sel = 0;
    g_scroll = 0;
    anim_set(&g_hl, 0);
    anim_set(&g_scroll_px, 0);
    /* g_install_* deliberately survive: an install started on a previous
     * visit may still be running, and tick() reports it when it lands. */
    set_state(PLAY_LOADING);
    schedule_job(JOB_CATALOGUE);
}

static void play_leave(void)
{
    if (g_state == PLAY_INSTALL && !g_install_inflight) {
        /* Popped (HOME) between download attempts: don't strand the card. */
        overlay_progress_finish(false, "Cancelled");
    }
    install_report();
    g_job = JOB_NONE;
    catalog_session_close();
}

static const shell_screen_t g_play_screen = {
    .name = "akiraplay",
    .title = "AKIRA PLAY",
    .flags = SCR_F_STATUSBAR | SCR_F_FOOTER,
    .hint_r = "B:Back",
    .enter = play_enter,
    .leave = play_leave,
    .on_key = play_key,
    .tick = play_tick,
    .draw = play_draw,
};

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void akiraplay_open(void)
{
    screen_push(&g_play_screen, NULL);
}
