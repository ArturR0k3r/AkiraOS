/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_apps_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_apps_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file apps_screen.c
 * @brief Installed app list with size/version and uninstall via confirm dialog.
 */

#include <zephyr/kernel.h>
#include <lvgl.h>

#include "../shell_theme.h"
#include "apps_screen.h"
#include <drivers/display/lvgl_input_driver.h>
#include <runtime/app_manager/app_manager.h>

static lv_obj_t *g_screen;
static lv_obj_t *g_list;

/* Pending uninstall name */
static char g_pending_uninstall_name[APP_NAME_MAX_LEN];

/* ------------------------------------------------------------------ */
/* Confirm dialog                                                       */
/* ------------------------------------------------------------------ */

static void confirm_yes_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    int ret = app_manager_uninstall(g_pending_uninstall_name);
    if (ret < 0) {
        LOG_ERR("Uninstall '%s' failed: %d", g_pending_uninstall_name, ret);
    } else {
        LOG_INF("App '%s' uninstalled", g_pending_uninstall_name);
    }

    /* Close dialog and refresh list */
    lv_obj_t *dialog = lv_event_get_user_data(e);
    lv_obj_del(dialog);

    /* Rebuild list */
    lv_obj_clean(g_list);

    app_info_t apps[CONFIG_AKIRA_APP_MAX_INSTALLED];
    int count = app_manager_list(apps, CONFIG_AKIRA_APP_MAX_INSTALLED);
    if (count <= 0) {
        lv_list_add_text(g_list, "No apps installed");
        return;
    }

    for (int i = 0; i < count; i++) {
        char entry[80];
        snprintf(entry, sizeof(entry), "%s  v%s  %u KB",
                 apps[i].name, apps[i].version,
                 (apps[i].size + 1023U) / 1024U);
        lv_obj_t *btn = lv_list_add_btn(g_list, NULL, entry);
        lv_obj_add_style(btn, &g_style_list_item, 0);
        lv_obj_set_user_data(btn, (void *)(uintptr_t)apps[i].id);
    }
}

static void confirm_no_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *dialog = lv_event_get_user_data(e);
    lv_obj_del(dialog);
}

static void open_confirm_dialog(const char *app_name)
{
    strncpy(g_pending_uninstall_name, app_name, APP_NAME_MAX_LEN - 1);
    g_pending_uninstall_name[APP_NAME_MAX_LEN - 1] = '\0';

    /* Semi-transparent overlay */
    lv_obj_t *overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay, SHELL_SCREEN_W, SHELL_SCREEN_H);
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);

    /* Dialog card */
    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_size(card, 240, 100);
    lv_obj_center(card);
    lv_obj_add_style(card, &g_style_card, 0);

    lv_obj_t *msg = lv_label_create(card);
    lv_label_set_text_fmt(msg, "Uninstall \"%s\"?", app_name);
    lv_obj_set_style_text_font(msg, SHELL_FONT_SMALL, 0);
    lv_obj_set_width(msg, 220);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *yes_btn = lv_btn_create(card);
    lv_obj_set_size(yes_btn, 90, 30);
    lv_obj_align(yes_btn, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    lv_obj_add_event_cb(yes_btn, confirm_yes_cb, LV_EVENT_CLICKED,
                        overlay);
    lv_obj_t *yes_lbl = lv_label_create(yes_btn);
    lv_label_set_text(yes_lbl, "Uninstall");
    lv_obj_center(yes_lbl);

    lv_obj_t *no_btn = lv_btn_create(card);
    lv_obj_set_size(no_btn, 90, 30);
    lv_obj_align(no_btn, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    lv_obj_add_event_cb(no_btn, confirm_no_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *no_lbl = lv_label_create(no_btn);
    lv_label_set_text(no_lbl, "Cancel");
    lv_obj_center(no_lbl);
}

/* ------------------------------------------------------------------ */
/* List item long-press (Y = LV_KEY_END)                               */
/* ------------------------------------------------------------------ */

static void list_key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY) {
        return;
    }
    uint32_t key = lv_indev_get_key(lv_indev_get_act());
    if (key != LV_KEY_END) { /* Y button → uninstall */
        return;
    }

    lv_obj_t *focused = lv_group_get_focused(lv_group_get_default());
    if (!focused) {
        return;
    }

    const char *btn_text = lv_list_get_btn_text(g_list, focused);
    /* Extract pure app name (before "  v" in formatted entry) */
    char pure_name[APP_NAME_MAX_LEN];
    const char *end = strstr(btn_text, "  v");
    size_t len = end ? (size_t)(end - btn_text) : strlen(btn_text);
    if (len >= APP_NAME_MAX_LEN) {
        len = APP_NAME_MAX_LEN - 1;
    }
    strncpy(pure_name, btn_text, len);
    pure_name[len] = '\0';
    open_confirm_dialog(pure_name);
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

    shell_theme_make_header(g_screen, "Apps");
    shell_theme_make_footer(g_screen, "B:Back", "Y:Uninstall");

    g_list = lv_list_create(g_screen);
    lv_obj_set_size(g_list, SHELL_SCREEN_W, SHELL_CONTENT_H);
    lv_obj_align(g_list, LV_ALIGN_TOP_LEFT, 0, SHELL_HEADER_H);
    lv_obj_set_style_border_width(g_list, 0, 0);
    lv_obj_set_style_pad_all(g_list, 0, 0);

    /* Populate */
    app_info_t apps[CONFIG_AKIRA_APP_MAX_INSTALLED];
    int count = app_manager_list(apps, CONFIG_AKIRA_APP_MAX_INSTALLED);

    if (count <= 0) {
        lv_list_add_text(g_list, "No apps installed");
    } else {
        lv_group_t *grp = lv_group_create();
        for (int i = 0; i < count; i++) {
            char entry[80];
            snprintf(entry, sizeof(entry), "%s  v%s  %u KB",
                     apps[i].name, apps[i].version,
                     (apps[i].size + 1023U) / 1024U);
            lv_obj_t *btn = lv_list_add_btn(g_list, NULL, entry);
            lv_obj_add_style(btn, &g_style_list_item, 0);
            lv_obj_set_user_data(btn, (void *)(uintptr_t)apps[i].id);
            lv_group_add_obj(grp, btn);
        }
        lv_indev_set_group(lvgl_input_get_keypad(), grp);
        lv_obj_add_event_cb(g_list, list_key_cb, LV_EVENT_KEY, NULL);
    }

    lv_obj_add_event_cb(g_screen, back_event_cb, LV_EVENT_KEY, NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void apps_screen_load(void)
{
    if (!g_screen) {
        build_screen();
    }
    lv_scr_load(g_screen);
}
