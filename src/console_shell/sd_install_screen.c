/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_sd_install
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_sd_install, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file sd_install_screen.c
 * @brief SD card WASM app browser — one screen on the shell's screen stack.
 *
 * Scans /SD:/apps/ for *.wasm / *.aot files (after extracting any .akpkg
 * archives in place), lists them, and runs the selected one straight from
 * the card with app_manager_run_from_sd() (SD XIP) on A-press.
 *
 * Layout (320x240):
 *   y=  0..25   Status bar (screen framework)
 *   y= 30..211  Scrollable list of dither cards (ROW_H=34 px per row)
 *   y=216..239  Footer hints (screen framework): "A:Run  B:Back"
 */

#include "sd_install_screen.h"
#include "akira_os_shell.h"
#include "shell_theme.h"
#include "ui/akira_ui.h"
#include "ui/anim.h"
#include "ui/overlay.h"
#include "ui/screen.h"

#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <runtime/app_manager/app_manager.h>
#include <storage/fs_manager.h>
#include <storage/sd_card.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

/* ---- List geometry (same rhythm as ui/list_screen.c) -------------- */
#define ROW_H 34
#define ROW_TOP (SCREEN_CONTENT_Y + 2)
#define ROW_X 12
#define ROW_W (SCR_W - 30) /* leaves a lane for the scrollbar */
#define CARD_H (ROW_H - 6)
#define VISIBLE_ROWS ((SCREEN_CONTENT_B - ROW_TOP) / ROW_H)
#define BAND_MID ((SCREEN_CONTENT_Y + SCREEN_CONTENT_B) / 2)
#define SEL_MS 130

/* ---- SD scan state ---------------------------------------------- */
#define SD_APPS_DIR "/SD:/apps"
#define MAX_SD_APPS 16

typedef struct
{
    char name[64];
    char path[128];
    uint32_t size;
} sd_entry_t;

typedef enum
{
    SD_NOCARD = 0, /* no card at open: message + B */
    SD_SCANNING,   /* scan queued; tick() runs it once this frame is up */
    SD_LIST,
} sd_state_t;

static sd_entry_t g_entries[MAX_SD_APPS] __attribute__((section(".ext_ram.bss")));
static int g_entry_count;
static int g_sel;
static int g_scroll;       /* first fully visible row */
static anim_t g_hl;        /* highlight position, px from the list's origin */
static anim_t g_scroll_px; /* scroll position, px */
static sd_state_t g_state;
static bool g_scan_shown; /* the "Reading SD card" frame has been painted */

/* ---- Run work ---------------------------------------------------- */
static char g_install_path[128] __attribute__((section(".ext_ram.bss")));

/* Set by the work item when the SD run fails, reported by the shell thread
 * (tick() or leave(), whichever comes first) as an error toast. */
static volatile int g_run_err;

/* Runs on the system workqueue: the shell thread is dormant (display handed
 * to the app), so hand the failure back through the shell's thread-safe
 * go-home event — it reclaims the display and pops us, and leave() toasts. */
static void sd_run_failed(int err)
{
    g_run_err = err;
    akira_os_shell_go_home();
}

static void do_install_work(struct k_work *work)
{
    ARG_UNUSED(work);
    LOG_INF("SD XIP: running %s", g_install_path);

    /* Shell already released display in akira_shell_set_wasm_launching().
     * Flush a blank frame so display SPI config is re-asserted before
     * the app's first draw call. */
    akira_display_clear(C_BLACK);
    akira_display_flush();

#if defined(CONFIG_AKIRA_SD_XIP)
    int ret = app_manager_run_from_sd(g_install_path);

    if (ret < 0)
    {
        LOG_ERR("SD XIP failed: %d", ret);
        sd_run_failed(ret);
    }
#else
    LOG_ERR("SD XIP not enabled");
    sd_run_failed(-ENOTSUP);
#endif
}

K_WORK_DEFINE(g_sd_install_work, do_install_work);

/* errno from app_manager_run_from_sd() → toast second line */
static const char *run_err_text(int err)
{
    switch (err)
    {
    case -EEXIST:
        return "Installed - start it from Home";
    case -EBUSY:
        return "Already running";
    case -ENOENT:
        return "Not found on the SD card";
    case -EFBIG:
        return "Too large to run from SD";
    case -ENOMEM:
    case -ENOSPC:
        return "Not enough memory";
    case -ENOTSUP:
        return "SD apps not in this build";
    default:
        return NULL;
    }
}

static void report_run_failure(void)
{
    int err = g_run_err;

    if (err == 0)
        return;
    g_run_err = 0;

    const char *why = run_err_text(err);
    char buf[24];

    if (!why)
    {
        snprintf(buf, sizeof(buf), "Error %d", err);
        why = buf;
    }
    overlay_toast(TOAST_ICON_ERROR, "Couldn't run app", why, 2500);
}

/* ---- Draw helpers ----------------------------------------------- */
static void draw_centred(int y, const char *s, uint16_t fg)
{
    akira_display_text((SCR_W - akira_ui_text_w(s, AKIRA_UI_FONT_SMALL)) / 2, y, s, fg);
}

/* Text cut to @p max_w px with a ".." tail. */
static void draw_clipped(int x, int y, int max_w, const char *s, uint16_t fg)
{
    char buf[48];
    int max_chars = MAX(max_w / AKIRA_UI_CELL_W, 3);

    if ((int)strlen(s) > max_chars)
    {
        snprintf(buf, sizeof(buf), "%.*s..", MIN(max_chars - 2, (int)sizeof(buf) - 3), s);
        s = buf;
    }
    akira_display_text(x, y, s, fg);
}

static bool row_off_band(int y)
{
    return y + CARD_H < SCREEN_CONTENT_Y - 4 || y > SCREEN_CONTENT_B;
}

static void draw_row(int i, int y, bool inverted)
{
    uint16_t fg = inverted ? C_BLACK : C_WHITE;
    int ty = y + (CARD_H - 10) / 2;
    char sz[16];

    /* Size (right) */
    if (g_entries[i].size >= 1024)
    {
        snprintf(sz, sizeof(sz), "%uKB", (unsigned)(g_entries[i].size / 1024));
    }
    else
    {
        snprintf(sz, sizeof(sz), "%uB", (unsigned)g_entries[i].size);
    }
    int sx = ROW_X + ROW_W - 10 - akira_ui_text_w(sz, AKIRA_UI_FONT_SMALL);
    akira_display_text(sx, ty, sz, fg);

    /* Name (left), clipped short of the size */
    draw_clipped(ROW_X + 10, ty, sx - 8 - (ROW_X + 10), g_entries[i].name, fg);
}

static void draw_list(void)
{
    if (g_entry_count == 0)
    {
        draw_centred(BAND_MID - 12, "No apps found", C_WHITE);
        draw_centred(BAND_MID + 2, "in /SD:/apps/", C_WHITE);
        return;
    }

    int scroll = anim_value(&g_scroll_px);
    int hl = anim_value(&g_hl);
    int first = MAX(scroll / ROW_H - 1, 0);
    int last = MIN(first + VISIBLE_ROWS + 2, g_entry_count);

    /* Idle cards first, then the sliding highlight over them, then labels. */
    for (int i = first; i < last; i++)
    {
        int y = ROW_TOP + i * ROW_H - scroll;
        if (!row_off_band(y))
            akira_ui_dither_card(ROW_X, y, ROW_W, CARD_H, 8, false, 2);
    }
    akira_ui_dither_card(ROW_X, ROW_TOP + hl - scroll, ROW_W, CARD_H, 8, true, 3);
    for (int i = first; i < last; i++)
    {
        int y = ROW_TOP + i * ROW_H - scroll;
        if (!row_off_band(y))
            draw_row(i, y, abs(hl - i * ROW_H) < ROW_H / 2);
    }

    /* Scrollbar lane — only when the list overflows */
    if (g_entry_count > VISIBLE_ROWS)
    {
        int track_y = ROW_TOP, track_h = VISIBLE_ROWS * ROW_H - 6;
        int max_scroll = (g_entry_count - VISIBLE_ROWS) * ROW_H;
        int knob_h = MAX(track_h * VISIBLE_ROWS / g_entry_count, 12);
        int knob_y = track_y + (track_h - knob_h) * CLAMP(scroll, 0, max_scroll) / max_scroll;

        akira_display_vline(SCR_W - 9, track_y, track_h, C_DKGRAY);
        akira_display_rounded_rect_fill(SCR_W - 11, knob_y, 5, knob_h, 2, C_WHITE);
    }
}

static void sd_draw(void)
{
    switch (g_state)
    {
    case SD_NOCARD:
        draw_centred(BAND_MID - 12, "No SD card detected", C_WHITE);
        draw_centred(BAND_MID + 2, "Insert card and reboot", C_WHITE);
        break;
    case SD_SCANNING:
        draw_centred(BAND_MID - 5, "Reading SD card...", C_GRAY);
        g_scan_shown = true; /* tick() may now run the scan */
        break;
    case SD_LIST:
        draw_list();
        break;
    }
}

/* ---- Scan ------------------------------------------------------- */
static void scan_sd(void)
{
    g_entry_count = 0;

    /* Decompress any .akpkg archives into plain .wasm/.aot first. */
    app_manager_extract_sd_akpkgs(SD_APPS_DIR);

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);

    int ret = fs_opendir(&dir, SD_APPS_DIR);
    if (ret < 0)
    {
        LOG_WRN("SD:/apps not accessible: %d", ret);
        return;
    }

    struct fs_dirent entry;
    while (g_entry_count < MAX_SD_APPS &&
           fs_readdir(&dir, &entry) == 0 &&
           entry.name[0] != '\0')
    {
        LOG_INF("Found SD entry: %s (type %d, size %zu)", entry.name, entry.type, entry.size);
        int nl = (int)strlen(entry.name);
        bool is_wasm = (nl >= 6 && strcasecmp(&entry.name[nl - 5], ".wasm") == 0);
        bool is_aot = (nl >= 5 && strcasecmp(&entry.name[nl - 4], ".aot") == 0);
        if (entry.type != FS_DIR_ENTRY_FILE || (!is_wasm && !is_aot))
        {
            continue;
        }
        strncpy(g_entries[g_entry_count].name, entry.name,
                sizeof(g_entries[0].name) - 1);
        snprintf(g_entries[g_entry_count].path,
                 sizeof(g_entries[0].path),
                 "%s/%s", SD_APPS_DIR, entry.name);
        g_entries[g_entry_count].size = (uint32_t)entry.size;
        g_entry_count++;
    }
    fs_closedir(&dir);
    LOG_INF("SD scan: %d apps (.wasm/.aot)", g_entry_count);
}

/* screen_run_busy() body: re-probe the card, then scan it. */
static void scan_job(void *arg)
{
    ARG_UNUSED(arg);

    /* Re-probe SD to recover from SPI state after display activity */
    akira_sd_card_deinit();
    if (akira_sd_card_init() == 0)
    {
        fs_manager_reinit_sd();
    }
    scan_sd();
}

/* ---- Behaviour -------------------------------------------------- */
static void set_state(sd_state_t s)
{
    g_state = s;
    if (s == SD_LIST && g_entry_count > 0)
        screen_set_hints("A:Run", "B:Back");
    else
        screen_set_hints(NULL, "B:Back");
    screen_invalidate();
}

static void move_sel(int to)
{
    if (g_entry_count == 0)
        return;
    to = CLAMP(to, 0, g_entry_count - 1);
    if (to == g_sel)
        return;
    g_sel = to;
    anim_to(&g_hl, to * ROW_H, SEL_MS, ANIM_EASE_OUT);

    int scroll = g_scroll;
    if (to < scroll)
        scroll = to;
    else if (to >= scroll + VISIBLE_ROWS)
        scroll = to - VISIBLE_ROWS + 1;
    if (scroll != g_scroll)
    {
        g_scroll = scroll;
        anim_to(&g_scroll_px, scroll * ROW_H, SEL_MS + 40, ANIM_EASE_OUT);
    }
    screen_invalidate();
}

static void run_selected(void)
{
    if (g_entry_count == 0 || g_sel >= g_entry_count)
        return;

    strncpy(g_install_path, g_entries[g_sel].path, sizeof(g_install_path) - 1);
    g_install_path[sizeof(g_install_path) - 1] = '\0';
    g_run_err = 0;
    /* Silence shell before work item fires — prevents the shell drawing
     * into the gap between A-press and app start. We stay on the stack:
     * when the app exits the shell resumes with this list on top. */
    akira_shell_set_wasm_launching();
    k_work_submit(&g_sd_install_work);
}

/* A short HOME edge is ignored: HOME long-press is the shell's, and it pops
 * us (leave() then reports anything pending). */
static void sd_key(uint32_t just)
{
    if (just & BIT(AKIRA_BTN_B))
    {
        screen_pop();
        return;
    }
    if (g_state != SD_LIST)
        return;

    if (just & BIT(AKIRA_BTN_UP))
        move_sel(g_sel - 1);
    if (just & BIT(AKIRA_BTN_DOWN))
        move_sel(g_sel + 1);
    if (just & BIT(AKIRA_BTN_A))
        run_selected();
}

static bool sd_tick(void)
{
    report_run_failure();

    if (g_state == SD_SCANNING && g_scan_shown)
    {
        screen_run_busy("Scanning SD card", scan_job, NULL);
        set_state(SD_LIST);
        return true;
    }
    return anim_running(&g_hl) || anim_running(&g_scroll_px);
}

static void sd_enter(void *arg)
{
    ARG_UNUSED(arg);
    g_entry_count = 0;
    g_sel = 0;
    g_scroll = 0;
    anim_set(&g_hl, 0);
    anim_set(&g_scroll_px, 0);
    g_scan_shown = false;

    /* Check SD card presence via FS manager before doing anything */
    set_state(fs_manager_sd_available() ? SD_SCANNING : SD_NOCARD);
}

static void sd_leave(void)
{
    report_run_failure();
}

static const shell_screen_t g_sd_screen = {
    .name = "sd_install",
    .title = "SD APPS",
    .flags = SCR_F_STATUSBAR | SCR_F_FOOTER,
    .hint_r = "B:Back",
    .enter = sd_enter,
    .leave = sd_leave,
    .on_key = sd_key,
    .tick = sd_tick,
    .draw = sd_draw,
};

/* ---- Public API ------------------------------------------------- */
void sd_install_open(void)
{
    screen_push(&g_sd_screen, NULL);
}
