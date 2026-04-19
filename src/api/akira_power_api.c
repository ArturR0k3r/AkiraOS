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
#include <errno.h>
#include <string.h>

/* Packed battery-status buffer layout (12 bytes total). */
#define BATT_BUF_MIN_LEN 12

/* flags byte bit definitions */
#define BATT_FLAG_CHARGING     BIT(0)
#define BATT_FLAG_LOW_BATTERY  BIT(1)

/* -------------------------------------------------------------------------
 * Read-only
 * ---------------------------------------------------------------------- */

int akira_native_power_get_mode(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_READ, -EACCES);
    return (int)akira_pm_get_mode();
}

int akira_native_power_get_battery_level(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_READ, -EACCES);

    uint8_t pct = 0;
    int ret = akira_pm_get_battery_level(&pct);
    if (ret < 0) {
        LOG_DBG("Battery level unavailable: %d", ret);
        return ret;
    }
    return (int)pct;
}

int akira_native_power_get_battery_status(wasm_exec_env_t exec_env,
                                          uint8_t *buf, uint32_t buf_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_READ, -EACCES);

    if (!buf || buf_len < BATT_BUF_MIN_LEN) {
        LOG_ERR("Invalid buffer: buf=%p len=%u (need %d)",
                buf, buf_len, BATT_BUF_MIN_LEN);
        return -EINVAL;
    }

    akira_battery_status_t s;
    int ret = akira_pm_get_battery_status(&s);
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

int akira_native_power_set_mode(wasm_exec_env_t exec_env, int mode)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_CTRL, -EACCES);

    if (mode < POWER_MODE_ACTIVE || mode > POWER_MODE_HIBERNATE) {
        LOG_ERR("power_set_mode: invalid mode %d", mode);
        return -EINVAL;
    }
    return akira_pm_set_mode((akira_power_mode_t)mode);
}

int akira_native_power_wake_on_gpio(wasm_exec_env_t exec_env,
                                    uint32_t pin, int edge)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_CTRL, -EACCES);
    return akira_pm_wake_on_gpio(pin, edge);
}

int akira_native_power_wake_on_timer(wasm_exec_env_t exec_env, uint32_t ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_CTRL, -EACCES);
    return akira_pm_wake_on_timer(ms);
}

int akira_native_power_set_low_power(wasm_exec_env_t exec_env, int enable)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_POWER_CTRL, -EACCES);
    return akira_pm_enable_low_power_mode(enable != 0);
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
