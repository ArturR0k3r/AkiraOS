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
 * Setting the clock needs "rtc.write" and a time >= 0 (1970..2038).  Alarms
 * are per app and cleared when the app ends.
 *
 * Gate: CONFIG_AKIRA_WASM_RTC=y
 */

#ifdef CONFIG_AKIRA_WASM_RTC

#include "akira_rtc_api.h"
#include <akira_app_containment.h>
#include <runtime/security.h>
#include <runtime/akira_runtime.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <inttypes.h>

#if defined(CONFIG_RTC) && defined(CONFIG_AKIRA_RTC_DEVICE)
#include <zephyr/drivers/rtc.h>
#include <zephyr/sys/timeutil.h>
#include <string.h>
static const struct device *g_rtc_dev;
#endif

/* Software clock, shared by all apps: Unix time = g_epoch_offset + uptime
 * seconds, once set.  The lock keeps the 64-bit offset from tearing on
 * 32-bit CPUs. */
static struct k_spinlock g_epoch_lock;
static int64_t g_epoch_offset;    /* seconds */
static bool    g_epoch_set;

/* One-shot alarm per app slot (uptime ms deadline, 0 = none), so one app
 * can neither clear nor consume another app's alarm. */
static struct k_spinlock g_alarm_lock;
static int64_t g_alarm_deadline_ms[AKIRA_MAX_WASM_INSTANCES];
static ATOMIC_DEFINE(g_alarm_fired, AKIRA_MAX_WASM_INSTANCES);

static int rtc_slot(wasm_exec_env_t exec_env)
{
    int slot = akira_runtime_slot_of_exec_env(exec_env);

    return (slot >= 0 && slot < AKIRA_MAX_WASM_INSTANCES) ? slot : -1;
}

/* ── internal init ──────────────────────────────────────────────────────── */

static void rtc_api_late_init(void)
{
#if defined(CONFIG_RTC) && defined(CONFIG_AKIRA_RTC_DEVICE)
    if (!g_rtc_dev) {
        const struct device *dev = DEVICE_DT_GET(DT_ALIAS(rtc0));

        if (device_is_ready(dev)) {
            g_rtc_dev = dev;
        } else {
            LOG_WRN_RATELIMIT("RTC device not ready, using uptime fallback");
        }
    }
#endif
}

#if defined(CONFIG_RTC) && defined(CONFIG_AKIRA_RTC_DEVICE)
/* Unix time (>= 0) to broken-down UTC.  Self-contained: gmtime() returns a
 * shared static buffer, and gmtime_r() is not visible with every libc.
 * Days to civil date after H. Hinnant's days_from_civil inverse. */
static void unix_to_rtc_time(int64_t t, struct rtc_time *rtm)
{
    static const uint16_t cum_days[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334,
    };
    int64_t days = t / 86400;
    int32_t rem = (int32_t)(t % 86400);

    memset(rtm, 0, sizeof(*rtm));
    rtm->tm_hour = rem / 3600;
    rtm->tm_min = (rem % 3600) / 60;
    rtm->tm_sec = rem % 60;
    rtm->tm_wday = (int)((days + 4) % 7); /* 1970-01-01 was a Thursday */

    int64_t z = days + 719468;
    int64_t era = z / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t mday = doy - (153 * mp + 2) / 5 + 1;
    uint32_t mon = (mp < 10) ? mp + 3 : mp - 9;           /* 1..12 */
    int64_t year = (int64_t)yoe + era * 400 + (mon <= 2);
    bool leap = (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));

    rtm->tm_year = (int)(year - 1900);
    rtm->tm_mon = (int)mon - 1;
    rtm->tm_mday = (int)mday;
    rtm->tm_yday = cum_days[mon - 1] + (int)mday - 1 + ((leap && mon > 2) ? 1 : 0);
    rtm->tm_isdst = -1;
}
#endif

/* ── get_unix_time ──────────────────────────────────────────────────────── */

int akira_native_rtc_get_unix_time(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RTC_READ, -EPERM);
    rtc_api_late_init();

#if defined(CONFIG_RTC) && defined(CONFIG_AKIRA_RTC_DEVICE)
    if (g_rtc_dev) {
        struct rtc_time rtm = {0};

        /* The driver holds its bus lock for the transfer. */
        akira_native_critical_enter(exec_env);
        int ret = rtc_get_time(g_rtc_dev, &rtm);
        akira_native_critical_exit(exec_env);
        if (ret == 0) {
            /* Convert rtc_time to seconds since epoch using Zephyr helper */
            time_t t = timeutil_timegm(rtc_time_to_tm(&rtm));
            return (int)t;
        }
    }
#endif
    /* Software fallback */
    k_spinlock_key_t key = k_spin_lock(&g_epoch_lock);
    bool set = g_epoch_set;
    int64_t offset = g_epoch_offset;
    k_spin_unlock(&g_epoch_lock, key);

    if (!set) {
        return 0;
    }
    int64_t up_s = k_uptime_get() / 1000;
    return (int)(offset + up_s);
}

/* ── get_uptime_ms ──────────────────────────────────────────────────────── */

int akira_native_rtc_get_uptime_ms(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RTC_READ, -EPERM);
    return (int)(k_uptime_get() & 0x7FFFFFFF);
}

/* ── set_unix_time ──────────────────────────────────────────────────────── */

int akira_native_rtc_set_unix_time(wasm_exec_env_t exec_env, int32_t unix_time)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RTC_WRITE, -EPERM);

    /* Before 1970 is no valid wall-clock time to set (int32 caps 2038). */
    if (unix_time < 0) {
        return -EINVAL;
    }
    rtc_api_late_init();

#if defined(CONFIG_RTC) && defined(CONFIG_AKIRA_RTC_DEVICE)
    if (g_rtc_dev) {
        struct rtc_time rtm;

        unix_to_rtc_time((int64_t)unix_time, &rtm);
        akira_native_critical_enter(exec_env);
        int ret = rtc_set_time(g_rtc_dev, &rtm);
        akira_native_critical_exit(exec_env);
        if (ret < 0) {
            LOG_WRN_RATELIMIT("rtc_set_time failed: %d", ret);
        }
    }
#endif
    /* Always keep the software fallback in sync */
    int64_t up_s   = k_uptime_get() / 1000;
    int64_t offset = (int64_t)unix_time - up_s;
    k_spinlock_key_t key = k_spin_lock(&g_epoch_lock);
    g_epoch_offset = offset;
    g_epoch_set    = true;
    k_spin_unlock(&g_epoch_lock, key);

    /* App-triggered: rate-limited so a looping app cannot flood the log. */
    LOG_INF_RATELIMIT("RTC set to %d (software epoch offset %" PRId64 ")",
                      unix_time, offset);
    return 0;
}

/* ── set_alarm ──────────────────────────────────────────────────────────── */

int akira_native_rtc_set_alarm(wasm_exec_env_t exec_env, int32_t deadline_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RTC_WRITE, -EPERM);

    if (deadline_ms <= 0) {
        return -EINVAL;
    }
    int slot = rtc_slot(exec_env);
    if (slot < 0) {
        return -EPERM;
    }

    k_spinlock_key_t key = k_spin_lock(&g_alarm_lock);
    g_alarm_deadline_ms[slot] = (int64_t)deadline_ms;
    atomic_clear_bit(g_alarm_fired, slot);
    k_spin_unlock(&g_alarm_lock, key);
    LOG_DBG("RTC alarm set for uptime %d ms (slot %d)", deadline_ms, slot);
    return 0;
}

/* ── alarm_fired ────────────────────────────────────────────────────────── */

int akira_native_rtc_alarm_fired(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_RTC_READ, -EPERM);

    int slot = rtc_slot(exec_env);
    if (slot < 0) {
        return 0;
    }

    k_spinlock_key_t key = k_spin_lock(&g_alarm_lock);
    if (g_alarm_deadline_ms[slot] > 0 &&
        k_uptime_get() >= g_alarm_deadline_ms[slot]) {
        g_alarm_deadline_ms[slot] = 0;
        atomic_set_bit(g_alarm_fired, slot);
    }
    k_spin_unlock(&g_alarm_lock, key);
    return atomic_test_and_clear_bit(g_alarm_fired, slot) ? 1 : 0;
}

/* ── App teardown ───────────────────────────────────────────────────────── */

/* An app's alarm goes with it: the next app in the slot starts clean. */
static void rtc_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);

    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&g_alarm_lock);
    g_alarm_deadline_ms[slot] = 0;
    atomic_clear_bit(g_alarm_fired, slot);
    k_spin_unlock(&g_alarm_lock, key);
}

AKIRA_APP_RESOURCE_DEFINE(rtc, 50, NULL, rtc_release);

#endif /* CONFIG_AKIRA_WASM_RTC */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_RTC))
#include <akira_native_registry.h>

/* rtc.*: get/set unix time, uptime, alarm */
static const NativeSymbol akira_rtc_natives[] = {
    {"rtc_get_unix_time",  (void *)akira_native_rtc_get_unix_time,  "()i",  NULL},
    {"rtc_get_uptime_ms",  (void *)akira_native_rtc_get_uptime_ms,  "()i",  NULL},
    {"rtc_set_unix_time",  (void *)akira_native_rtc_set_unix_time,  "(i)i", NULL},
    {"rtc_set_alarm",      (void *)akira_native_rtc_set_alarm,      "(i)i", NULL},
    {"rtc_alarm_fired",    (void *)akira_native_rtc_alarm_fired,    "()i",  NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_rtc_api, "env", akira_rtc_natives);
#endif
