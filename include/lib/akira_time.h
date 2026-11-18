/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_TIME_H
#define AKIRA_TIME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return the current epoch time in seconds.
 *
 * If the clock has been set via akira_time_set_epoch(), returns the real
 * wall-clock time derived from the stored offset.  Otherwise returns
 * (k_uptime_get() / 1000), i.e. seconds since boot.
 * @stability stable
 * @since 1.3
 */
int64_t akira_time_get_epoch(void);

/**
 * @brief Set the real-wall-clock epoch (seconds since Unix epoch, UTC).
 *
 * Persists the calibration offset to NVS so it survives reboots.
 */
void akira_time_set_epoch(int64_t epoch_s);

/**
 * @brief Returns true if the real clock has been set at least once
 *        (either via akira_time_set_epoch() or NVS restore on boot).
 */
bool akira_time_is_set(void);

/**
 * @brief Get the stored UTC offset in seconds (default 0 = UTC).
 *
 * Apply to akira_time_get_epoch() to obtain local wall-clock time.
 * E.g. UTC+3 → returns 10800.
 */
int32_t akira_time_get_tz_offset_s(void);

/**
 * @brief Set and persist the UTC offset in seconds.
 *
 * Pass 0 for UTC, 3*3600 for UTC+3, -5*3600 for UTC-5, etc.
 * Persisted to NVS under "system/tz_offset".
 */
void akira_time_set_tz_offset_s(int32_t offset_s);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_TIME_H */
