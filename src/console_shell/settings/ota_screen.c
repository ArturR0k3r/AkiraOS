/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_ota_screen
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_ota_screen, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file ota_screen.c
 * @brief Settings → Software update: what's installed and how to update it.
 */

#include <zephyr/kernel.h>
#include <stdio.h>

#include "ota_screen.h"

static void val_version(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
    snprintf(buf, len, "v%s", CONFIG_AKIRA_OS_VERSION);
}

static void val_built(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
    snprintf(buf, len, "%s", __DATE__);
}

static void val_method(const list_item_t *it, char *buf, size_t len)
{
    ARG_UNUSED(it);
#if defined(CONFIG_AKIRA_OTA)
    snprintf(buf, len, "Web app upload");
#else
    snprintf(buf, len, "Not in this build");
#endif
}

static const list_item_t g_items[] = {
    { .kind = LI_INFO, .label = "Installed", .value = val_version },
    { .kind = LI_INFO, .label = "Built", .value = val_built },
    { .kind = LI_INFO, .label = "Update via", .value = val_method },
};

const list_model_t ota_page = {
    .title = "Software update",
    .items = g_items,
    .count = ARRAY_SIZE(g_items),
};
