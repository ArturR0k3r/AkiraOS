/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_display_own
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_display_own, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file display_ownership.c
 * @brief Display ownership arbitration between the OS shell and WASM apps.
 */

#include "display_ownership.h"
#include <zephyr/kernel.h>

#if defined(CONFIG_LVGL)
#include <lvgl.h>
#endif

/* Mutex protecting g_owner transitions */
static K_MUTEX_DEFINE(g_display_mutex);

/* Current owner — shell owns the display at boot */
static display_owner_t g_owner = DISPLAY_OWNER_SHELL;

/* ------------------------------------------------------------------ */

int akira_display_claim_shell(void)
{
    int ret = k_mutex_lock(&g_display_mutex, K_MSEC(500));

    if (ret < 0) {
        LOG_ERR("Display claim timed out: current owner=%d", (int)g_owner);
        return ret;
    }

    g_owner = DISPLAY_OWNER_SHELL;
    k_mutex_unlock(&g_display_mutex);

#if defined(CONFIG_LVGL)
    /* Resume LVGL's periodic tick so it can refresh the screen */
    lv_timer_handler();
#endif

    LOG_DBG("Display claimed by OS shell");
    return 0;
}

void akira_display_release_to_wasm(void)
{
    k_mutex_lock(&g_display_mutex, K_FOREVER);

#if defined(CONFIG_LVGL)
    /*
     * Flush any pending LVGL draw operations before handing over.
     * This prevents tearing when the WASM app immediately clears the screen.
     */
    lv_timer_handler();
#endif

    g_owner = DISPLAY_OWNER_WASM;
    k_mutex_unlock(&g_display_mutex);

    LOG_DBG("Display released to WASM app");
}

display_owner_t akira_display_get_owner(void)
{
    return g_owner;
}
