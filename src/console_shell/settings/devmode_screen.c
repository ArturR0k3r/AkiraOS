/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_devmode_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_devmode_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file devmode_screen.c
 * @brief Settings → Developer: developer-mode toggle plus build details.
 */

#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <settings/settings.h>
#include <stdio.h>

#include "devmode_screen.h"

static bool g_devmode_enabled;

static void devmode_enter(void)
{
    char sv[4] = "";

    akira_settings_get("akira/devmode/enabled", sv, sizeof(sv));
    g_devmode_enabled = (sv[0] == '1');
}

static int get_devmode(const list_item_t *it)
{
    ARG_UNUSED(it);
    return g_devmode_enabled;
}

static void set_devmode(const list_item_t *it, int v)
{
    ARG_UNUSED(it);
    g_devmode_enabled = v;
    akira_settings_set("akira/devmode/enabled", v ? "1" : "0", 0);
}

static void val_os(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
    snprintf(buf, len, "v%s", CONFIG_AKIRA_OS_VERSION);
}

static void val_zephyr(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
    snprintf(buf, len, "%d.%d.%d", (int)KERNEL_VERSION_MAJOR,
             (int)KERNEL_VERSION_MINOR, (int)KERNEL_PATCHLEVEL);
}

static void val_board(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
    snprintf(buf, len, "%s", CONFIG_BOARD);
}

static const list_item_t g_items[] = {
    { .kind = LI_TOGGLE, .label = "Developer mode", .get = get_devmode,
      .set = set_devmode },
    { .kind = LI_INFO, .label = "AkiraOS", .value = val_os },
    { .kind = LI_INFO, .label = "Zephyr", .value = val_zephyr },
    { .kind = LI_INFO, .label = "Board", .value = val_board },
};

const list_model_t devmode_page = {
    .title = "Developer",
    .items = g_items,
    .count = ARRAY_SIZE(g_items),
    .on_enter = devmode_enter,
};
