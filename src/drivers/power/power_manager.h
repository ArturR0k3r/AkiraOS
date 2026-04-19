/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file power_manager.h
 * @brief AkiraOS Power Management — sleep modes, battery, wake sources.
 */

#ifndef AKIRA_POWER_MANAGER_H
#define AKIRA_POWER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief System power modes (ordered from most to least active). */
typedef enum {
    POWER_MODE_ACTIVE      = 0, /**< Full speed, all peripherals on.                */
    POWER_MODE_IDLE,            /**< CPU clock gated when idle, RAM on.             */
    POWER_MODE_LIGHT_SLEEP,     /**< CPU off, RAM retained, fast wakeup.            */
    POWER_MODE_DEEP_SLEEP,      /**< CPU + RAM off, only RTC domain retained.       */
    POWER_MODE_HIBERNATE,       /**< Everything off except RTC alarm / ext reset.   */
} akira_power_mode_t;

/** @brief Bitmask of enabled wake sources. */
typedef enum {
    WAKE_SOURCE_NONE  = 0,
    WAKE_SOURCE_GPIO  = (1 << 0),
    WAKE_SOURCE_TIMER = (1 << 1),
    WAKE_SOURCE_UART  = (1 << 2),
    WAKE_SOURCE_BT    = (1 << 3),
    WAKE_SOURCE_WIFI  = (1 << 4),
    WAKE_SOURCE_ULP   = (1 << 5),
} akira_wake_source_t;

/** @brief Per-app power policy hint. */
typedef enum {
    POWER_POLICY_DEFAULT     = 0,
    POWER_POLICY_PERFORMANCE,   /**< Keep CPU running at full speed. */
    POWER_POLICY_BALANCED,      /**< Allow idle sleep between tasks.  */
    POWER_POLICY_LOW_POWER,     /**< Aggressive power saving.         */
} akira_power_policy_t;

/**
 * @brief Live battery status snapshot.
 *
 * All electrical values use integer millivolts / milliamps to avoid
 * floating-point in WASM-facing code paths.
 */
typedef struct {
    uint8_t  level_percent; /**< State of charge 0-100 %.                 */
    int32_t  voltage_mv;    /**< Bus / pack voltage in millivolts.         */
    int32_t  current_ma;    /**< Charge(+) / discharge(-) current in mA.  */
    bool     charging;      /**< True when an external charger is active.  */
    bool     low_battery;   /**< True when SoC < CONFIG_AKIRA_BATTERY_LOW_THRESHOLD. */
} akira_battery_status_t;

/**
 * @brief Initialize power manager and bind hardware (fuel gauge / INA219).
 * @return 0 on success, negative errno otherwise.
 */
int power_manager_init(void);

/**
 * @brief Transition to a new power mode.
 *
 * Delegates to Zephyr PM state forcing.  Deep sleep and hibernate require
 * CONFIG_AKIRA_POWER_DEEP_SLEEP=y.
 *
 * @param mode Target power mode.
 * @return 0 on success, -ENOTSUP if mode is disabled, -EINVAL on bad arg.
 */
int akira_pm_set_mode(akira_power_mode_t mode);

/** @brief Return the current power mode. */
akira_power_mode_t akira_pm_get_mode(void);

/**
 * @brief Register a GPIO pin as a wakeup source.
 * @param pin  GPIO pin number.
 * @param edge 0=low, 1=high, 2=any edge.
 * @return 0 on success.
 */
int akira_pm_wake_on_gpio(uint32_t pin, int edge);

/**
 * @brief Register a timer wakeup after @p ms milliseconds.
 * @return 0 on success, -EINVAL if ms == 0.
 */
int akira_pm_wake_on_timer(uint32_t ms);

/**
 * @brief Read battery state of charge.
 * @param[out] percent  0-100 %.
 * @return 0 on success, -ENODEV if no battery hardware is present.
 */
int akira_pm_get_battery_level(uint8_t *percent);

/**
 * @brief Read full battery status snapshot.
 * @param[out] status Filled on success.
 * @return 0 on success, -ENODEV if no battery hardware is present.
 */
int akira_pm_get_battery_status(akira_battery_status_t *status);

/**
 * @brief Enable or disable automatic low-power idle management.
 * @return 0 always.
 */
int akira_pm_enable_low_power_mode(bool enable);

/**
 * @brief Register a power policy for a named app / container.
 * @return 0 on success, -ENOMEM if the policy table is full.
 */
int akira_pm_set_policy(const char *name, akira_power_policy_t policy);

/** @brief Return the most performance-demanding policy across all registered apps. */
akira_power_policy_t akira_pm_get_aggregate_policy(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_POWER_MANAGER_H */
