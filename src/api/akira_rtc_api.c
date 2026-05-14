/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_rtc_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_rtc_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_rtc_api.c
 * @brief Real-time clock WASM native API.
 *
 * RTC hardware is accessed via the Zephyr RTC driver API when
 * CONFIG_RTC=y and the "rtc0" device alias is configured.
 * On boards without an RTC (e.g., native_sim) all time operations
 * fall back gracefully to k_uptime_get().
 *
 * Gate: CONFIG_AKIRA_WASM_RTC=y
 */

#ifdef CONFIG_AKIRA_WASM_RTC

#include "akira_rtc_api.h"
#include <runtime/security.h>
#include <runtime/akira_runtime.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <stdatomic.h>

#if defined(CONFIG_RTC) && defined(CONFIG_AKIRA_RTC_DEVICE)
#include <zephyr/drivers/rtc.h>
static const struct device *g_rtc_dev;
#endif

/* Unix epoch offset — set by akira_native_rtc_set_unix_time().
 * Stores: (unix_time_at_set - uptime_ms_at_set / 1000) */
static int64_t g_epoch_offset;    /* seconds */
static bool    g_epoch_set;

/* Alarm state */
static int64_t  g_alarm_deadline_ms;
static atomic_t g_alarm_fired;

/* ── internal init ──────────────────────────────────────────────────────── */

static void rtc_api_late_init(void)
{
#if defined(CONFIG_RTC) && defined(CONFIG_AKIRA_RTC_DEVICE)
    if (!g_rtc_dev) {
        g_rtc_dev = DEVICE_DT_GET(DT_ALIAS(rtc0));
        if (!device_is_ready(g_rtc_dev)) {
            LOG_WRN("RTC device not ready, using uptime fallback");
            g_rtc_dev = NULL;
        }
    }
#endif
}

/* ── get_unix_time ──────────────────────────────────────────────────────── */

int akira_native_rtc_get_unix_time(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RTC_READ, -EACCES);
    rtc_api_late_init();

#if defined(CONFIG_RTC) && defined(CONFIG_AKIRA_RTC_DEVICE)
    if (g_rtc_dev) {
        struct rtc_time rtm;
        if (rtc_get_time(g_rtc_dev, &rtm) == 0) {
            /* Convert rtc_time to seconds since epoch using Zephyr helper */
            time_t t = timeutil_timegm((struct tm *)&rtm);
            return (int)t;
        }
    }
#endif
    /* Software fallback */
    if (!g_epoch_set) {
        return 0;
    }
    int64_t up_s = k_uptime_get() / 1000;
    return (int)(g_epoch_offset + up_s);
}

/* ── get_uptime_ms ──────────────────────────────────────────────────────── */

int akira_native_rtc_get_uptime_ms(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RTC_READ, -EACCES);
    return (int)(k_uptime_get() & 0x7FFFFFFF);
}

/* ── set_unix_time ──────────────────────────────────────────────────────── */

int akira_native_rtc_set_unix_time(wasm_exec_env_t exec_env, int32_t unix_time)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RTC_WRITE, -EACCES);
    rtc_api_late_init();

#if defined(CONFIG_RTC) && defined(CONFIG_AKIRA_RTC_DEVICE)
    if (g_rtc_dev) {
        struct tm *utc = gmtime((time_t *)&unix_time);
        struct rtc_time rtm;
        memcpy(&rtm, utc, sizeof(struct tm));
        int ret = rtc_set_time(g_rtc_dev, &rtm);
        if (ret < 0) {
            LOG_WRN("rtc_set_time failed: %d", ret);
        }
    }
#endif
    /* Always keep the software fallback in sync */
    int64_t up_s    = k_uptime_get() / 1000;
    g_epoch_offset  = (int64_t)unix_time - up_s;
    g_epoch_set     = true;
    LOG_INF("RTC set to %d (software epoch offset %" PRId64 ")",
            unix_time, g_epoch_offset);
    return 0;
}

/* ── set_alarm ──────────────────────────────────────────────────────────── */

int akira_native_rtc_set_alarm(wasm_exec_env_t exec_env, int32_t deadline_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RTC_WRITE, -EACCES);

    if (deadline_ms <= 0) {
        return -EINVAL;
    }
    g_alarm_deadline_ms = (int64_t)deadline_ms;
    atomic_store(&g_alarm_fired, 0);
    LOG_DBG("RTC alarm set for uptime %" PRId64 " ms", g_alarm_deadline_ms);
    return 0;
}

/* ── alarm_fired ────────────────────────────────────────────────────────── */

int akira_native_rtc_alarm_fired(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RTC_READ, -EACCES);

    if (g_alarm_deadline_ms > 0 &&
        k_uptime_get() >= g_alarm_deadline_ms) {
        g_alarm_deadline_ms = 0;
        atomic_store(&g_alarm_fired, 1);
    }
    return (int)atomic_exchange(&g_alarm_fired, 0);
}

#endif /* CONFIG_AKIRA_WASM_RTC */
