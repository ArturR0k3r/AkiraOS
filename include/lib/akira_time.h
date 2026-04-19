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

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_TIME_H */
