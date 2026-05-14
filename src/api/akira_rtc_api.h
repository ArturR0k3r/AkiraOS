/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_rtc_api.h
 * @brief Real-time clock WASM native API.
 *
 * Functions: get_unix_time, get_uptime_ms, set_unix_time, set_alarm.
 * Falls back to Zephyr k_uptime_get() if no RTC device is configured.
 *
 * Gate: CONFIG_AKIRA_WASM_RTC=y
 * Capabilities: AKIRA_CAP_RTC_READ (bit 29), AKIRA_CAP_RTC_WRITE (bit 30)
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_RTC_API_H
#define AKIRA_RTC_API_H

#include <wasm_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get current Unix time (seconds since 1970-01-01T00:00:00Z).
 * Returns 0 if the RTC has not been set (device just booted).
 * @return Seconds since epoch as int32 (wraps 2038 — adequate for embedded use).
 */
int akira_native_rtc_get_unix_time(wasm_exec_env_t exec_env);

/**
 * @brief Get system uptime in milliseconds (k_uptime_get()).
 * Always available; does not require RTC hardware.
 * @return Uptime in ms, truncated to int32 (wraps after ~24 days).
 */
int akira_native_rtc_get_uptime_ms(wasm_exec_env_t exec_env);

/**
 * @brief Set the RTC to a Unix timestamp.
 * Requires AKIRA_CAP_RTC_WRITE.
 * @param unix_time  Seconds since epoch.
 * @return 0 on success, -ENODEV if no RTC hardware, -EACCES if no capability.
 */
int akira_native_rtc_set_unix_time(wasm_exec_env_t exec_env, int32_t unix_time);

/**
 * @brief Set a one-shot alarm callback (fires once at deadline_ms uptime).
 * WASM cannot register a callback directly; the alarm fires a pending flag
 * readable via akira_native_rtc_alarm_fired().
 * @param deadline_ms  Uptime ms at which alarm should fire.
 * @return 0 on success, negative errno on error.
 */
int akira_native_rtc_set_alarm(wasm_exec_env_t exec_env, int32_t deadline_ms);

/**
 * @brief Check if the last alarm has fired.
 * @return 1 if fired (flag cleared after read), 0 if not, negative on error.
 */
int akira_native_rtc_alarm_fired(wasm_exec_env_t exec_env);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_RTC_API_H */
