/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_power_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_power_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_power_api.c
 * @brief WASM-facing power / battery native API.
 *
 * Read-only functions need AKIRA_CAP_POWER_READ.
 * Control functions need AKIRA_CAP_POWER_CTRL (and CONFIG_AKIRA_WASM_POWER_CONTROL=y).
 */

#include "akira_power_api.h"
#include "akira_api.h"
#include <runtime/security.h>
#include <drivers/power/power_manager.h>
#include <akira_app_containment.h>
#include <lib/mem_helper.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <errno.h>
#include <string.h>

/* Packed battery-status buffer layout (12 bytes total). */
#define BATT_BUF_MIN_LEN 12

/* flags byte bit definitions */
#define BATT_FLAG_CHARGING     BIT(0)
#define BATT_FLAG_LOW_BATTERY  BIT(1)

/* -------------------------------------------------------------------------
 * Battery reads
 *
 * The fuel gauge / INA219 / ADC sits on a bus the OS itself uses (on most
 * boards the system I2C). An app polling the battery in a loop must not
 * hammer it: a reading is reused for BATT_CACHE_MS, so apps cause at most
 * one hardware read per interval whatever they do. The read holds the bus
 * lock, so it is a native critical section.
 * ---------------------------------------------------------------------- */

#define BATT_CACHE_MS 1000

struct batt_cache {
    int64_t level_ms;
    int64_t status_ms;
    int level_ret;
    int status_ret;
    uint8_t level;
    bool level_valid;
    bool status_valid;
    akira_battery_status_t status;
};

static struct batt_cache AKIRA_BULK_BSS s_batt;
static struct k_spinlock s_batt_lock;

static bool batt_fresh(int64_t at_ms, bool valid, int64_t now)
{
    return valid && now - at_ms < BATT_CACHE_MS;
}

static int batt_level(wasm_exec_env_t exec_env, uint8_t *pct)
{
    int64_t now = k_uptime_get();
    int ret;

    k_spinlock_key_t key = k_spin_lock(&s_batt_lock);
    if (batt_fresh(s_batt.level_ms, s_batt.level_valid, now)) {
        ret = s_batt.level_ret;
        *pct = s_batt.level;
        k_spin_unlock(&s_batt_lock, key);
        return ret;
    }
    k_spin_unlock(&s_batt_lock, key);

    akira_native_critical_enter(exec_env);
    ret = akira_pm_get_battery_level(pct);
    akira_native_critical_exit(exec_env);

    key = k_spin_lock(&s_batt_lock);
    s_batt.level_ms = now;
    s_batt.level_ret = ret;
    s_batt.level = *pct;
    s_batt.level_valid = true;
    k_spin_unlock(&s_batt_lock, key);
    return ret;
}

static int batt_status(wasm_exec_env_t exec_env, akira_battery_status_t *st)
{
    int64_t now = k_uptime_get();
    int ret;

    k_spinlock_key_t key = k_spin_lock(&s_batt_lock);
    if (batt_fresh(s_batt.status_ms, s_batt.status_valid, now)) {
        ret = s_batt.status_ret;
        *st = s_batt.status;
        k_spin_unlock(&s_batt_lock, key);
        return ret;
    }
    k_spin_unlock(&s_batt_lock, key);

    akira_native_critical_enter(exec_env);
    ret = akira_pm_get_battery_status(st);
    akira_native_critical_exit(exec_env);

    key = k_spin_lock(&s_batt_lock);
    s_batt.status_ms = now;
    s_batt.status_ret = ret;
    s_batt.status = *st;
    s_batt.status_valid = true;
    k_spin_unlock(&s_batt_lock, key);
    return ret;
}

/* -------------------------------------------------------------------------
 * Read-only
 * ---------------------------------------------------------------------- */

int akira_native_power_get_mode(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_READ, -EPERM);
    return (int)akira_pm_get_mode();
}

int akira_native_power_get_battery_level(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_READ, -EPERM);

    uint8_t pct = 0;
    int ret = batt_level(exec_env, &pct);
    if (ret < 0) {
        LOG_DBG("Battery level unavailable: %d", ret);
        return ret;
    }
    return (int)pct;
}

/* Registered "(*~)i": WAMR validated [buf, buf + buf_len). */
int akira_native_power_get_battery_status(wasm_exec_env_t exec_env,
                                          uint8_t *buf, uint32_t buf_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_READ, -EPERM);

    if (!buf || buf_len < BATT_BUF_MIN_LEN) {
        LOG_DBG("Invalid buffer: len=%u (need %d)", buf_len, BATT_BUF_MIN_LEN);
        return -EINVAL;
    }

    akira_battery_status_t s;
    int ret = batt_status(exec_env, &s);
    if (ret < 0) {
        return ret;
    }

    /* Pack into WASM-friendly layout (no floats, explicit byte order). */
    memset(buf, 0, BATT_BUF_MIN_LEN);
    buf[0] = s.level_percent;
    buf[1] = (s.charging    ? BATT_FLAG_CHARGING    : 0)
           | (s.low_battery ? BATT_FLAG_LOW_BATTERY : 0);
    /* bytes [2-3] reserved / padding */
    /* int32 voltage_mv at offset 4 (little-endian) */
    buf[4] = (uint8_t)(s.voltage_mv & 0xFF);
    buf[5] = (uint8_t)((s.voltage_mv >>  8) & 0xFF);
    buf[6] = (uint8_t)((s.voltage_mv >> 16) & 0xFF);
    buf[7] = (uint8_t)((s.voltage_mv >> 24) & 0xFF);
    /* int32 current_ma at offset 8 */
    buf[8]  = (uint8_t)(s.current_ma & 0xFF);
    buf[9]  = (uint8_t)((s.current_ma >>  8) & 0xFF);
    buf[10] = (uint8_t)((s.current_ma >> 16) & 0xFF);
    buf[11] = (uint8_t)((s.current_ma >> 24) & 0xFF);

    return 0;
}

/* -------------------------------------------------------------------------
 * Control  (CONFIG_AKIRA_WASM_POWER_CONTROL=y required)
 * ---------------------------------------------------------------------- */

#ifdef CONFIG_AKIRA_WASM_POWER_CONTROL

/* Control calls change system state and log it: a burst of
 * POWER_CTRL_BURST, then one per POWER_CTRL_PERIOD_MS, for all apps
 * together. Over that, -EAGAIN. */
#define POWER_CTRL_BURST     8
#define POWER_CTRL_PERIOD_MS 250

static struct k_spinlock s_ctrl_lock;
static int64_t s_ctrl_last_ms;
static int s_ctrl_tokens = POWER_CTRL_BURST;
static int s_last_wake_pin = -1;
static int s_last_wake_edge = -1;
static uint32_t s_last_wake_ms;
static int s_last_low_power = -1;

static bool power_ctrl_allowed(void)
{
    int64_t now = k_uptime_get();
    bool ok;

    k_spinlock_key_t key = k_spin_lock(&s_ctrl_lock);
    int64_t refill = (now - s_ctrl_last_ms) / POWER_CTRL_PERIOD_MS;
    if (refill > 0) {
        s_ctrl_tokens = (int)MIN((int64_t)s_ctrl_tokens + refill, (int64_t)POWER_CTRL_BURST);
        s_ctrl_last_ms += refill * POWER_CTRL_PERIOD_MS;
    }
    ok = s_ctrl_tokens > 0;
    if (ok) {
        s_ctrl_tokens--;
    }
    k_spin_unlock(&s_ctrl_lock, key);
    return ok;
}

int akira_native_power_set_mode(wasm_exec_env_t exec_env, int mode)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_CTRL, -EPERM);

    if (mode < POWER_MODE_ACTIVE || mode > POWER_MODE_HIBERNATE) {
        LOG_DBG("power_set_mode: invalid mode %d", mode);
        return -EINVAL;
    }

    /* An app may only choose modes the system always wakes from by itself.
     * Light sleep and deeper need a wake source, and the power manager only
     * records wake-source intent (akira_pm_wake_on_gpio/timer arm nothing),
     * so an app could put the device to sleep with no way back short of a
     * reset. Those transitions stay with the OS. */
    if (mode >= POWER_MODE_LIGHT_SLEEP) {
        akira_native_critical_enter(exec_env); /* log line: console lock */
        LOG_WRN_RATELIMIT_RATE(5000, "power_set_mode(%d): sleep modes are not available to apps",
                               mode);
        akira_native_critical_exit(exec_env);
        return -ENOTSUP;
    }
    if ((int)akira_pm_get_mode() == mode) {
        return 0;
    }
    if (!power_ctrl_allowed()) {
        return -EAGAIN;
    }
    /* The power manager logs each change (console lock in immediate mode). */
    akira_native_critical_enter(exec_env);
    int ret = akira_pm_set_mode((akira_power_mode_t)mode);
    akira_native_critical_exit(exec_env);
    return ret;
}

int akira_native_power_wake_on_gpio(wasm_exec_env_t exec_env,
                                    uint32_t pin, int edge)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_CTRL, -EPERM);

    if (pin > 63 || edge < 0 || edge > 2) {
        return -EINVAL;
    }
    /* Records intent only (no pin is touched); repeats are no-ops. */
    k_spinlock_key_t key = k_spin_lock(&s_ctrl_lock);
    bool same = s_last_wake_pin == (int)pin && s_last_wake_edge == edge;
    k_spin_unlock(&s_ctrl_lock, key);
    if (same) {
        return 0;
    }
    if (!power_ctrl_allowed()) {
        return -EAGAIN;
    }
    akira_native_critical_enter(exec_env); /* logs the change */
    int ret = akira_pm_wake_on_gpio(pin, edge);
    akira_native_critical_exit(exec_env);
    if (ret == 0) {
        key = k_spin_lock(&s_ctrl_lock);
        s_last_wake_pin = (int)pin;
        s_last_wake_edge = edge;
        k_spin_unlock(&s_ctrl_lock, key);
    }
    return ret;
}

int akira_native_power_wake_on_timer(wasm_exec_env_t exec_env, uint32_t ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_CTRL, -EPERM);

    if (ms == 0) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&s_ctrl_lock);
    bool same = s_last_wake_ms == ms;
    k_spin_unlock(&s_ctrl_lock, key);
    if (same) {
        return 0;
    }
    if (!power_ctrl_allowed()) {
        return -EAGAIN;
    }
    akira_native_critical_enter(exec_env); /* logs the change */
    int ret = akira_pm_wake_on_timer(ms);
    akira_native_critical_exit(exec_env);
    if (ret == 0) {
        key = k_spin_lock(&s_ctrl_lock);
        s_last_wake_ms = ms;
        k_spin_unlock(&s_ctrl_lock, key);
    }
    return ret;
}

int akira_native_power_set_low_power(wasm_exec_env_t exec_env, int enable)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_CTRL, -EPERM);

    int want = enable != 0;
    k_spinlock_key_t key = k_spin_lock(&s_ctrl_lock);
    bool same = s_last_low_power == want;
    k_spin_unlock(&s_ctrl_lock, key);
    if (same) {
        return 0;
    }
    if (!power_ctrl_allowed()) {
        return -EAGAIN;
    }
    akira_native_critical_enter(exec_env); /* logs the change */
    int ret = akira_pm_enable_low_power_mode(want != 0);
    akira_native_critical_exit(exec_env);
    if (ret == 0) {
        key = k_spin_lock(&s_ctrl_lock);
        s_last_low_power = want;
        k_spin_unlock(&s_ctrl_lock, key);
    }
    return ret;
}

#else /* !CONFIG_AKIRA_WASM_POWER_CONTROL — stub out with -ENOTSUP */

int akira_native_power_set_mode(wasm_exec_env_t exec_env, int mode)
{
    ARG_UNUSED(exec_env); ARG_UNUSED(mode);
    return -ENOTSUP;
}

int akira_native_power_wake_on_gpio(wasm_exec_env_t exec_env,
                                    uint32_t pin, int edge)
{
    ARG_UNUSED(exec_env); ARG_UNUSED(pin); ARG_UNUSED(edge);
    return -ENOTSUP;
}

int akira_native_power_wake_on_timer(wasm_exec_env_t exec_env, uint32_t ms)
{
    ARG_UNUSED(exec_env); ARG_UNUSED(ms);
    return -ENOTSUP;
}

int akira_native_power_set_low_power(wasm_exec_env_t exec_env, int enable)
{
    ARG_UNUSED(exec_env); ARG_UNUSED(enable);
    return -ENOTSUP;
}

#endif /* CONFIG_AKIRA_WASM_POWER_CONTROL */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_POWER))
#include <akira_native_registry.h>

/* power.read: battery level/status, mode query */
static const NativeSymbol akira_power_natives[] = {
    {"power_get_mode", (void *)akira_native_power_get_mode, "()i", NULL},
    {"power_get_battery_level", (void *)akira_native_power_get_battery_level, "()i", NULL},
    {"power_get_battery_status", (void *)akira_native_power_get_battery_status, "(*~)i", NULL},
    /* power.control: mode transitions and wake sources (elevated) */
    {"power_set_mode", (void *)akira_native_power_set_mode, "(i)i", NULL},
    {"power_wake_on_gpio", (void *)akira_native_power_wake_on_gpio, "(ii)i", NULL},
    {"power_wake_on_timer", (void *)akira_native_power_wake_on_timer, "(i)i", NULL},
    {"power_set_low_power", (void *)akira_native_power_set_low_power, "(i)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_power_api, "env", akira_power_natives);
#endif
