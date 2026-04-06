/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_sd_install
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_sd_install, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file sd_install_screen.c
 * @brief SD card WASM app browser and one-touch installer.
 *
 * Scans /SD:/apps/ for *.wasm files, lists them name+size, and lets the
 * user install any app with a single A-press.  A manifest.json next to
 * the .wasm file is parsed automatically by app_manager_install_from_path().
 *
 * Flow:
 *   1. sd_install_screen_load() mounts FAT, scans /SD:/apps for .wasm files
 *   2. lv_list of found files (name + human-readable size)
 *   3. User selects entry → A → install_progress_screen overlaid
 *   4. app_manager_install_from_path() called in a work-queue item
 *   5. Progress screen closes on completion; HOME list refreshes
 */

#include "sd_install_screen.h"
#include "install_progress_screen.h"
#include "shell_theme.h"
#include "home_screen.h"


#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <runtime/app_manager/app_manager.h>

/* SD mount point (must match DTS / storage.c) */
#define SD_APPS_DIR "/SD:/apps"
#define MAX_SD_APPS  16

/* ------------------------------------------------------------------ */
/* Install work item (runs outside LVGL thread to avoid blocking UI)  */
/* ------------------------------------------------------------------ */

static char g_install_path[128];

static void do_install_work(struct k_work *work)
{
    ARG_UNUSED(work);

    LOG_INF("Installing from SD: %s", g_install_path);
    install_progress_show(NULL, 0, "Installing...");

    int ret = app_manager_install_from_path(g_install_path);
    if (ret < 0) {
        LOG_ERR("Install failed: %d", ret);
        install_progress_show(NULL, 100, "Install FAILED");
        k_sleep(K_SECONDS(2));
    } else {
        install_progress_show(NULL, 100, "Done!");
        k_sleep(K_MSEC(800));
    }

    install_progress_hide();
    home_screen_refresh();
    home_screen_load();
}

K_WORK_DEFINE(g_install_work, do_install_work);

/* ------------------------------------------------------------------ */
/* List item callback                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char path[128]; /* full SD path */
} sd_item_t;

static sd_item_t g_sd_items[MAX_SD_APPS];

static void item_clicked(lv_event_t *e)
{
    sd_item_t *item = lv_event_get_user_data(e);
    if (!item) return;

    strncpy(g_install_path, item->path, sizeof(g_install_path) - 1);
    k_work_submit(&g_install_work);
}

/* ------------------------------------------------------------------ */
/* Back button                                                         */
/* ------------------------------------------------------------------ */

static void back_btn_cb(lv_event_t *e)
{
    ARG_UNUSED(e);
    home_screen_load();
}

/* ------------------------------------------------------------------ */
/* Screen state                                                        */
/* ------------------------------------------------------------------ */

static lv_obj_t *g_screen;
static lv_obj_t *g_list;
static lv_group_t *g_group;

void sd_install_screen_create(void)
{
    g_screen = lv_obj_create(NULL);
    lv_obj_add_style(g_screen, &g_style_screen, 0);

    shell_theme_make_header(g_screen, "Install from SD");

    g_list = lv_list_create(g_screen);
    int list_y = SHELL_HEADER_H;
    int list_h = SHELL_SCREEN_H - list_y - SHELL_FOOTER_H;
    lv_obj_set_size(g_list, SHELL_SCREEN_W, list_h);
    lv_obj_set_pos(g_list, 0, list_y);
    lv_obj_set_style_bg_color(g_list, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_list, 0, 0);
    lv_obj_set_style_pad_all(g_list, 0, 0);
    lv_obj_set_style_radius(g_list, 0, 0);

    shell_theme_make_footer(g_screen, "B:Back", "A:Install");

    g_group = lv_group_create();
    lv_indev_set_group(lv_indev_get_next(NULL), g_group);

    /* Back button mapped to B key (LV_KEY_ESC) via group */
    lv_obj_add_event_cb(g_screen, back_btn_cb, LV_EVENT_KEY, NULL);
}

void sd_install_screen_load(void)
{
    if (!g_screen) {
        sd_install_screen_create();
    }

    /* Clear previous scan results */
    lv_obj_clean(g_list);
    lv_group_remove_all_objs(g_group);

    lv_scr_load(g_screen);

    /* Scan SD */
    struct fs_dir_t dir;
    fs_dir_t_init(&dir);

    int ret = fs_opendir(&dir, SD_APPS_DIR);
    if (ret < 0) {
        LOG_WRN("SD:/apps not accessible: %d", ret);
        lv_obj_t *lbl = lv_label_create(g_list);
        lv_label_set_text(lbl, "SD card not found or empty.\nInsert card with /apps/*.wasm");
        lv_obj_set_style_text_color(lbl, SHELL_COLOR_SUBTEXT, 0);
        lv_obj_set_style_text_font(lbl, SHELL_FONT_SMALL, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 12);
        return;
    }

    struct fs_dirent entry;
    int count = 0;

    while (count < MAX_SD_APPS && fs_readdir(&dir, &entry) == 0
           && entry.name[0] != '\0') {

        /* Filter for *.wasm files */
        int namelen = strlen(entry.name);
        if (entry.type != FS_DIR_ENTRY_FILE || namelen < 6 ||
            strcmp(&entry.name[namelen - 5], ".wasm") != 0) {
            continue;
        }

        snprintf(g_sd_items[count].path,
                 sizeof(g_sd_items[count].path),
                 "%s/%s", SD_APPS_DIR, entry.name);

        /* Human-readable size: show KB */
        char label[64];
        if (entry.size >= 1024) {
            snprintf(label, sizeof(label), "%s  (%u KB)",
                     entry.name, (unsigned)(entry.size / 1024));
        } else {
            snprintf(label, sizeof(label), "%s  (%u B)",
                     entry.name, (unsigned)entry.size);
        }

        lv_obj_t *btn = lv_list_add_btn(g_list, NULL, label);
        lv_obj_add_style(btn, &g_style_list_item, 0);
        lv_obj_add_event_cb(btn, item_clicked,
                            LV_EVENT_CLICKED, &g_sd_items[count]);
        lv_group_add_obj(g_group, btn);

        count++;
    }

    fs_closedir(&dir);

    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(g_list);
        lv_label_set_text(lbl, "No *.wasm files found\nin /SD:/apps/");
        lv_obj_set_style_text_color(lbl, SHELL_COLOR_SUBTEXT, 0);
        lv_obj_set_style_text_font(lbl, SHELL_FONT_SMALL, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 12);
    } else {
        LOG_INF("SD scan: %d WASM files found", count);
    }
}
