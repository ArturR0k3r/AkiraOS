/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file status_model.h
 * @brief What the status bar shows: clock, Wi-Fi/BT link, battery.
 *
 * Shared by every screen, so it lives outside any one of them.  Also runs
 * the one-shot SNTP sync when Wi-Fi first comes up.
 */

#ifndef AKIRA_SHELL_STATUS_MODEL_H
#define AKIRA_SHELL_STATUS_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef CONFIG_AKIRA_POWER_MANAGER
#include <drivers/power/power_manager.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char clock[6];       /**< "HH:MM" local time */
    bool time_set;       /**< wall clock known (else clock is uptime) */
    bool wifi_on;
    bool bt_on;
    bool batt_valid;     /**< a gauge reading exists */
    uint8_t batt_pct;
    uint8_t batt_state;  /**< akira_batt_state_t, or 0 without a power manager */
    bool batt_online;    /**< external power present */
    int32_t batt_ma;
    int32_t batt_mv;
    uint32_t batt_seq;
} shell_status_t;

/** Refresh clock + connectivity (call ~1 Hz). @return true if anything shown changed. */
bool status_model_update(void);

/** Refresh the battery fields from the cached monitor snapshot (cheap, any rate).
 *  @return true if anything shown changed. */
bool status_model_update_battery(void);

/** Current model. */
const shell_status_t *status_model_get(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SHELL_STATUS_MODEL_H */
