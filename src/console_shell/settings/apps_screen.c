/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_apps_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_apps_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file apps_screen.c
 * @brief Settings → Apps: installed apps with their size; A uninstalls
 *        (through the Capability-Guard confirmation).
 */

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdio.h>

#include <runtime/app_manager/app_manager.h>
#include "apps_screen.h"
#include "home_screen.h"
#include "ui/akira_ui.h"
#include "ui/overlay.h"

#define MAX_APPS CONFIG_AKIRA_APP_MAX_INSTALLED

static app_info_t g_apps[MAX_APPS] __attribute__((section(".ext_ram.bss")));
static list_item_t g_items[MAX_APPS + 1] __attribute__((section(".ext_ram.bss")));
static char g_question[48];

static void apps_rebuild(void);

static void val_size(const list_item_t *it, char *buf, size_t len)
{
    const app_info_t *a = &g_apps[(intptr_t)it->arg];

    snprintf(buf, len, "%uKB", (unsigned)((a->size + 1023) / 1024));
}

static void uninstall_done(bool confirmed, void *ctx)
{
    const app_info_t *a = &g_apps[(intptr_t)ctx];

    if (!confirmed) {
        return;
    }
    if (app_manager_uninstall(a->name) == 0) {
        overlay_toast(TOAST_ICON_OK, "Uninstalled", a->name, 2000);
    } else {
        overlay_toast(TOAST_ICON_ERROR, "Uninstall failed", a->name, 2500);
    }
    apps_rebuild();
    home_screen_refresh();
    screen_invalidate();
}

static void act_uninstall(const list_item_t *it)
{
    snprintf(g_question, sizeof(g_question), "Uninstall %s?",
             g_apps[(intptr_t)it->arg].name);
    akira_ui_confirm_open("APP_UNINSTALL", g_question, NULL, uninstall_done, it->arg);
}

static void apps_rebuild(void)
{
    int n = app_manager_list(g_apps, MAX_APPS);

    n = MAX(n, 0);
    for (int i = 0; i < n; i++) {
        g_items[i] = (list_item_t){
            .kind = LI_ACTION,
            .label = g_apps[i].name,
            .arg = (void *)(intptr_t)i,
            .action = act_uninstall,
            .value = val_size,
        };
    }
    if (n == 0) {
        g_items[0] = (list_item_t){ .kind = LI_INFO, .label = "No apps installed" };
    }
    apps_page.count = MAX(n, 1);
}

list_model_t apps_page = {
    .title = "Apps",
    .items = g_items,
    .count = 0,
    .on_enter = apps_rebuild,
    .hint_l = "A:Uninstall",
};
