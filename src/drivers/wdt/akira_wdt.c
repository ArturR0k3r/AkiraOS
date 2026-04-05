/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_wdt
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_wdt, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_wdt.c
 * @brief AkiraOS system watchdog manager.
 *
 * Finds the hardware WDT device via DT (first zephyr,watchdog node that is
 * enabled in the board overlay), installs a single timeout channel, enables
 * the watchdog, and auto-feeds it from a k_work_delayable at the configured
 * interval.
 *
 * All timing parameters come from Kconfig — no board-specific constants in
 * this file.  Board overlays enable the WDT node; board .conf files set:
 *   CONFIG_WATCHDOG=y
 *   CONFIG_AKIRA_WDT=y
 *   CONFIG_AKIRA_WDT_TIMEOUT_MS=<ms>       (default 30000)
 *   CONFIG_AKIRA_WDT_FEED_INTERVAL_MS=<ms> (default 10000)
 */

#include "akira_wdt.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <errno.h>

static const struct device *s_wdt_dev;
static int s_wdt_channel = -1;
static bool s_active;

static struct k_work_delayable s_feed_work;

/* -------------------------------------------------------------------------- */
/* Auto-feed worker                                                            */
/* -------------------------------------------------------------------------- */

static void wdt_feed_worker(struct k_work *work)
{
    ARG_UNUSED(work);

    if (s_active) {
        wdt_feed(s_wdt_dev, s_wdt_channel);
        LOG_DBG("WDT fed (auto)");
    }

    k_work_reschedule(&s_feed_work,
                      K_MSEC(CONFIG_AKIRA_WDT_FEED_INTERVAL_MS));
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

void akira_wdt_feed(void)
{
    if (s_active) {
        wdt_feed(s_wdt_dev, s_wdt_channel);
        LOG_DBG("WDT fed (manual)");
    }
}

bool akira_wdt_is_active(void)
{
    return s_active;
}

/* -------------------------------------------------------------------------- */
/* Initialisation                                                              */
/* -------------------------------------------------------------------------- */

int akira_wdt_init(void)
{
    s_wdt_dev = DEVICE_DT_GET_ANY(zephyr_watchdog);
    if (!s_wdt_dev || !device_is_ready(s_wdt_dev)) {
        LOG_WRN("WDT device not ready — watchdog disabled");
        return -ENODEV;
    }

    struct wdt_timeout_cfg cfg = {
        .window = {
            .min = 0,
            .max = CONFIG_AKIRA_WDT_TIMEOUT_MS,
        },
        .callback  = NULL,  /* hardware reset on expiry */
        .flags     = WDT_FLAG_RESET_SOC,
    };

    int ch = wdt_install_timeout(s_wdt_dev, &cfg);
    if (ch < 0) {
        LOG_ERR("WDT install timeout failed: %d", ch);
        return ch;
    }
    s_wdt_channel = ch;

    int ret = wdt_enable(s_wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (ret < 0) {
        /* Some drivers don't support the debug-pause option; retry plain. */
        ret = wdt_enable(s_wdt_dev, 0);
    }
    if (ret < 0) {
        LOG_ERR("WDT enable failed: %d", ret);
        return ret;
    }

    s_active = true;

    k_work_init_delayable(&s_feed_work, wdt_feed_worker);
    k_work_schedule(&s_feed_work,
                    K_MSEC(CONFIG_AKIRA_WDT_FEED_INTERVAL_MS));

    LOG_INF("WDT enabled: timeout=%d ms feed_interval=%d ms",
            CONFIG_AKIRA_WDT_TIMEOUT_MS,
            CONFIG_AKIRA_WDT_FEED_INTERVAL_MS);
    return 0;
}

SYS_INIT(akira_wdt_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
