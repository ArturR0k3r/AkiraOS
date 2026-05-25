/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_sd_install
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_sd_install, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file sd_install_screen.c
 * @brief SD card WASM app browser and one-touch installer — Liquid Crystal UI.
 *
 * Scans /SD:/apps/ for *.wasm files, renders a scrollable list panel,
 * and installs via app_manager_install_from_path() on A-press.
 * Pure akira_display_* renderer — no LVGL.
 *
 * Layout (320x240):
 *   y=  0..23   Status bar (dither + title)
 *   y= 24       Hairline separator
 *   y= 25..215  Scrollable file list (ITEM_H=32 px per row)
 *   y=215       Hairline separator
 *   y=216..239  Footer: "A - Install  |  B - Back"
 */

#include "sd_install_screen.h"
#include "install_progress_screen.h"
#include "home_screen.h"
#include "shell_theme.h"

#include <api/akira_display_api.h>
#include <api/akira_input_api.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <runtime/app_manager/app_manager.h>
#include <storage/fs_manager.h>
#include <storage/sd_card.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

/* SD install uses slightly different geometry than the standard settings list */
#undef LIST_Y
#define LIST_Y 25
#define LIST_H 190
#undef ITEM_H
#define ITEM_H 32

/* ---- SD scan state ---------------------------------------------- */
#define SD_APPS_DIR "/SD:/apps"
#define MAX_SD_APPS 16

typedef struct
{
    char name[64];
    char path[128];
    uint32_t size;
} sd_entry_t;

static sd_entry_t g_entries[MAX_SD_APPS];
static int g_entry_count;
static int g_sel;
static int g_scroll;
static bool g_active;

/* ---- Install work ----------------------------------------------- */
static char g_install_path[128];

static void do_install_work(struct k_work *work)
{
    ARG_UNUSED(work);
    LOG_INF("SD XIP: running %s", g_install_path);

    int ret = app_manager_run_from_sd(g_install_path);
    if (ret < 0)
    {
        LOG_ERR("SD XIP failed: %d", ret);
    }
    home_screen_load();
}

K_WORK_DEFINE(g_sd_install_work, do_install_work);

/* ---- Draw helpers ----------------------------------------------- */
static void draw_centred(int x, int y, int w, const char *s, uint16_t fg, uint16_t bg)
{
    int tw = (int)strlen(s) * 8;
    int lx = x + (tw < w ? (w - tw) / 2 : 0);
    akira_display_rect(x, y, w, 10, bg);
    akira_display_text(lx, y, s, fg);
}

static void draw_screen(void)
{
    akira_display_clear(C_BLACK);

    /* Status bar */
    akira_display_rect(0, 0, SCR_W, SBAR_H, C_BLACK);
    draw_centred(0, (SBAR_H - 10) / 2, SCR_W, "SD APPS", C_WHITE, C_BLACK);
    akira_display_hline(0, SBAR_H, SCR_W, C_WHITE);
    akira_display_hline(0, SBAR_H + 1, SCR_W, C_WHITE);

    /* List area */
    akira_display_rect(0, LIST_Y, SCR_W, LIST_H, C_BLACK);

    int vis = LIST_H / ITEM_H;

    if (g_entry_count == 0)
    {
        draw_centred(0, LIST_Y + LIST_H / 2 - 10, SCR_W,
                     "No apps found", C_GRAY, C_BLACK);
        draw_centred(0, LIST_Y + LIST_H / 2 + 4, SCR_W,
                     "in /SD:/apps/", C_DKGRAY, C_BLACK);
    }
    else
    {
        if (g_scroll > g_entry_count - vis)
            g_scroll = g_entry_count - vis;
        if (g_scroll < 0)
            g_scroll = 0;

        for (int i = g_scroll; i < g_entry_count && i < g_scroll + vis; i++)
        {
            int iy = LIST_Y + (i - g_scroll) * ITEM_H;
            bool hi = (i == g_sel);
            uint16_t ibg = hi ? C_WHITE : C_BLACK;
            uint16_t ifg = hi ? C_BLACK : C_WHITE;

            akira_display_rounded_rect_fill(ITEM_X, iy + 2, ITEM_W, ITEM_H - 4, 3, ibg);
            if (hi)
            {
                akira_display_rounded_rect(ITEM_X, iy + 2, ITEM_W, ITEM_H - 4, 3, C_BLACK);
                akira_display_rounded_rect(ITEM_X + 1, iy + 3, ITEM_W - 2, ITEM_H - 6, 2, C_BLACK);
            }
            else
            {
                akira_display_rounded_rect(ITEM_X, iy + 2, ITEM_W, ITEM_H - 4, 3, C_DKGRAY);
            }

            /* Name (left) */
            akira_display_text(ITEM_X + 6, iy + 2 + (ITEM_H - 4 - 10) / 2,
                               g_entries[i].name, ifg);

            /* Size (right) */
            char sz[16];
            if (g_entries[i].size >= 1024)
            {
                snprintf(sz, sizeof(sz), "%uKB", (unsigned)(g_entries[i].size / 1024));
            }
            else
            {
                snprintf(sz, sizeof(sz), "%uB", (unsigned)g_entries[i].size);
            }
            int tw = (int)strlen(sz) * 8;
            akira_display_text(ITEM_X + ITEM_W - tw - 6,
                               iy + 2 + (ITEM_H - 4 - 10) / 2,
                               sz, ifg);
        }

        /* Scroll indicators */
        if (g_scroll > 0)
        {
            akira_display_text(SCR_W - 12, LIST_Y + 2, "^", C_GRAY);
        }
        if (g_scroll + vis < g_entry_count)
        {
            akira_display_text(SCR_W - 12, LIST_Y + vis * ITEM_H + 2, "v", C_GRAY);
        }
    }

    /* Footer */
    akira_display_hline(0, FOOT_Y - 1, SCR_W, C_WHITE);
    akira_display_hline(0, FOOT_Y - 2, SCR_W, C_WHITE);
    akira_display_rect(0, FOOT_Y, SCR_W, FOOT_H, C_BLACK);
    akira_display_text(8, FOOT_Y + 7, "A-Run  |  B-Back", C_WHITE);

    akira_display_flush();
}

/* ---- Scan ------------------------------------------------------- */
static void scan_sd(void)
{
    g_entry_count = 0;

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

/* ---- Public API ------------------------------------------------- */
void sd_install_screen_create(void)
{
    g_entry_count = 0;
    g_sel = 0;
    g_scroll = 0;
    g_active = false;
}

void sd_install_screen_load(void)
{
    /* Check SD card presence via FS manager before doing anything */
    if (!fs_manager_sd_available())
    {
        akira_display_clear(C_BLACK);
        akira_display_rect(0, 0, SCR_W, SBAR_H, C_BLACK);
        draw_centred(0, (SBAR_H - 10) / 2, SCR_W, "SD APPS", C_WHITE, C_BLACK);
        akira_display_hline(0, SBAR_H, SCR_W, C_WHITE);
        akira_display_hline(0, SBAR_H + 1, SCR_W, C_WHITE);
        draw_centred(0, SCR_H / 2 - 10, SCR_W, "No SD card detected", C_GRAY, C_BLACK);
        draw_centred(0, SCR_H / 2 + 4, SCR_W, "Insert card and reboot", C_DKGRAY, C_BLACK);
        akira_display_hline(0, FOOT_Y - 1, SCR_W, C_WHITE);
        akira_display_hline(0, FOOT_Y - 2, SCR_W, C_WHITE);
        akira_display_rect(0, FOOT_Y, SCR_W, FOOT_H, C_BLACK);
        akira_display_text(8, FOOT_Y + 7, "B-Back", C_WHITE);
        akira_display_flush();
        /* Wait for B to go back */
        uint32_t prev = 0;
        while (true)
        {
            k_sleep(K_MSEC(20));
            uint32_t btns = akira_input_get_bitmask();
            uint32_t just = btns & ~prev;
            prev = btns;
            if (just & BIT(AKIRA_BTN_B))
            {
                home_screen_load();
                return;
            }
        }
    }

    g_active = true;
    g_sel = 0;
    g_scroll = 0;

    /* Re-probe SD to recover from SPI state after display activity */
    akira_sd_card_deinit();
    if (akira_sd_card_init() == 0)
    {
        fs_manager_reinit_sd();
    }

    scan_sd();
    draw_screen();

    /* Blocking event loop — returns when B pressed or install triggered */
    uint32_t prev_btns = 0;

    while (g_active)
    {
        k_sleep(K_MSEC(20));

        uint32_t btns = akira_input_get_bitmask();
        uint32_t just = btns & ~prev_btns;
        prev_btns = btns;

        if (!just)
            continue;

        int vis = LIST_H / ITEM_H;

        if (just & BIT(AKIRA_BTN_UP))
        {
            if (g_sel > 0)
            {
                g_sel--;
                if (g_sel < g_scroll)
                    g_scroll--;
                draw_screen();
            }
        }
        if (just & BIT(AKIRA_BTN_DOWN))
        {
            if (g_sel < g_entry_count - 1)
            {
                g_sel++;
                if (g_sel >= g_scroll + vis)
                    g_scroll++;
                draw_screen();
            }
        }
        if (just & BIT(AKIRA_BTN_A))
        {
            if (g_entry_count > 0 && g_sel < g_entry_count)
            {
                strncpy(g_install_path, g_entries[g_sel].path,
                        sizeof(g_install_path) - 1);
                k_work_submit(&g_sd_install_work);
                g_active = false;
            }
        }
        if ((just & BIT(AKIRA_BTN_B)) || (just & BIT(AKIRA_BTN_HOME)))
        {
            g_active = false;
            home_screen_load();
        }
    }
}
