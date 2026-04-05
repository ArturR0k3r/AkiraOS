/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_devmode_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_devmode_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file devmode_screen.c
 * @brief Developer mode toggle and runtime debug info panel.
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/version.h>
#include <lvgl.h>

#include "../shell_theme.h"
#include "devmode_screen.h"

static lv_obj_t *g_screen;
static lv_obj_t *g_sw;
static lv_obj_t *g_info_panel;

/* Persisted state */
static bool g_devmode_enabled;

/* ------------------------------------------------------------------ */
/* Info panel refresh                                                   */
/* ------------------------------------------------------------------ */

static void refresh_info_panel(void)
{
    if (!g_info_panel) {
        return;
    }

    lv_obj_clean(g_info_panel);

    if (!g_devmode_enabled) {
        lv_obj_t *lbl = lv_label_create(g_info_panel);
        lv_label_set_text(lbl, "Enable developer mode to\nsee debug information.");
        lv_obj_set_style_text_font(lbl, SHELL_FONT_SMALL, 0);
        lv_obj_set_style_text_color(lbl,
                                    lv_color_make(0x80, 0x80, 0x80), 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
        return;
    }

    /* Heap stats */
    char buf[256];
    int off = 0;

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
    {
        extern struct sys_heap _system_heap;
        struct sys_memory_stats stats;
        sys_heap_runtime_stats_get(&_system_heap, &stats);
        off += snprintf(buf + off, sizeof(buf) - off,
                        "Heap alloc: %zu B\n", stats.allocated_bytes);
        off += snprintf(buf + off, sizeof(buf) - off,
                        "Heap free:  %zu B\n", stats.free_bytes);
    }
#else
    off += snprintf(buf + off, sizeof(buf) - off, "Heap stats: N/A\n");
#endif

    off += snprintf(buf + off, sizeof(buf) - off,
                    "OS: %s\n", CONFIG_AKIRA_OS_VERSION);
    off += snprintf(buf + off, sizeof(buf) - off,
                    "Zephyr: %d.%d.%d\n",
                    (int)KERNEL_VERSION_MAJOR,
                    (int)KERNEL_VERSION_MINOR,
                    (int)KERNEL_PATCHLEVEL);

    lv_obj_t *lbl = lv_label_create(g_info_panel);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, SHELL_FONT_SMALL, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Toggle callback                                                      */
/* ------------------------------------------------------------------ */

static void sw_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    g_devmode_enabled = lv_obj_has_state(g_sw, LV_STATE_CHECKED);
    settings_save_one("akira/devmode/enabled",
                      &g_devmode_enabled, sizeof(g_devmode_enabled));
    LOG_INF("Developer mode %s", g_devmode_enabled ? "enabled" : "disabled");
    refresh_info_panel();
}

/* ------------------------------------------------------------------ */
/* Settings handler                                                     */
/* ------------------------------------------------------------------ */

static int settings_load_cb(const char *key, size_t len,
                             settings_read_cb read_cb, void *cb_arg,
                             void *param)
{
    ARG_UNUSED(param);

    if (strcmp(key, "enabled") == 0 && len == sizeof(bool)) {
        bool val;
        if (read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
            g_devmode_enabled = val;
        }
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(akira_devmode_sh, "akira/devmode",
                                NULL, settings_load_cb, NULL, NULL);

/* ------------------------------------------------------------------ */
/* Refresh button                                                       */
/* ------------------------------------------------------------------ */

static void refresh_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        refresh_info_panel();
    }
}

/* ------------------------------------------------------------------ */
/* Back key                                                             */
/* ------------------------------------------------------------------ */

static void back_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_KEY) {
        uint32_t key = lv_indev_get_key(lv_indev_get_act());
        if (key == LV_KEY_ESC) {
            extern void settings_screen_load(void);
            settings_screen_load();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Screen construction                                                  */
/* ------------------------------------------------------------------ */

static void build_screen(void)
{
    g_screen = lv_obj_create(NULL);
    lv_obj_add_style(g_screen, &g_style_screen, 0);

    shell_theme_make_header(g_screen, "Developer");
    shell_theme_make_footer(g_screen, "B:Back", "A:Refresh");

    int y = SHELL_HEADER_H + 10;

    /* Toggle row */
    lv_obj_t *lbl_en = lv_label_create(g_screen);
    lv_label_set_text(lbl_en, "Developer mode");
    lv_obj_add_style(lbl_en, &g_style_list_item, 0);
    lv_obj_align(lbl_en, LV_ALIGN_TOP_LEFT, 8, y);

    g_sw = lv_switch_create(g_screen);
    lv_obj_align(g_sw, LV_ALIGN_TOP_RIGHT, -8, y);
    lv_obj_add_event_cb(g_sw, sw_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    y += 36;

    /* Separator */
    lv_obj_t *sep = lv_obj_create(g_screen);
    lv_obj_set_size(sep, SHELL_SCREEN_W - 16, 1);
    lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_add_style(sep, &g_style_separator, 0);
    y += 8;

    /* Refresh button */
    lv_obj_t *ref_btn = lv_btn_create(g_screen);
    lv_obj_set_size(ref_btn, 100, 28);
    lv_obj_align(ref_btn, LV_ALIGN_TOP_RIGHT, -8, y);
    lv_obj_add_event_cb(ref_btn, refresh_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ref_lbl = lv_label_create(ref_btn);
    lv_label_set_text(ref_lbl, "Refresh");
    lv_obj_center(ref_lbl);
    y += 34;

    /* Info panel */
    g_info_panel = lv_obj_create(g_screen);
    lv_obj_set_size(g_info_panel, SHELL_SCREEN_W - 16,
                    SHELL_SCREEN_H - SHELL_HEADER_H - SHELL_FOOTER_H - y + SHELL_HEADER_H - 4);
    lv_obj_align(g_info_panel, LV_ALIGN_TOP_LEFT, 8, y);
    lv_obj_set_style_bg_color(g_info_panel,
                              lv_color_make(0xF8, 0xF8, 0xF8), 0);
    lv_obj_set_style_border_color(g_info_panel,
                                  lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_set_style_border_width(g_info_panel, 1, 0);
    lv_obj_set_style_pad_all(g_info_panel, 4, 0);

    lv_obj_add_event_cb(g_screen, back_event_cb, LV_EVENT_KEY, NULL);

    /* Load persisted state */
    settings_load_subtree("akira/devmode");
    if (g_devmode_enabled) {
        lv_obj_add_state(g_sw, LV_STATE_CHECKED);
    }
    refresh_info_panel();
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void devmode_screen_load(void)
{
    if (!g_screen) {
        build_screen();
    }
    lv_scr_load(g_screen);
}
