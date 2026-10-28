/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file power_manager.h
 * @brief AkiraOS Power Management — sleep modes, battery, wake sources.
 * @stability stable
 * @since 1.4
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
    uint8_t  level_percent;  /**< State of charge 0-100 %.                 */
    int32_t  voltage_mv;     /**< Bus / pack voltage in millivolts.         */
    int32_t  current_ma;     /**< Charge(+) / discharge(-) current in mA.  */
    bool     charging;       /**< True when an external charger is active.  */
    bool     low_battery;    /**< True when SoC < CONFIG_AKIRA_BATTERY_LOW_THRESHOLD. */
    int32_t  temperature_c;  /**< Battery temperature in whole degrees C, INT32_MIN if unavailable. */
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
 * @brief True while the charger IC reports an active charge cycle.
 *
 * Read straight from the charger (BQ25601 on AkiraConsole Production), so it
 * works even on builds with no fuel gauge, or when the gauge is not
 * responding.  Used to inhibit auto deep-sleep: powering the device off while
 * it sits on a charger is never what the user wants.
 *
 * @return true if charging; false if not charging, or if no charger is bound.
 */
bool akira_pm_is_charging(void);

/* ---------- cached battery monitor (UI-facing) ---------- */

/** @brief Charge state as the UI should present it. */
typedef enum {
    AKIRA_BATT_UNKNOWN = 0,   /**< No reading yet, or no battery hardware.          */
    AKIRA_BATT_DISCHARGING,   /**< Running from the cell.                           */
    AKIRA_BATT_CHARGING,      /**< External power, charge cycle active.             */
    AKIRA_BATT_FULL,          /**< External power, charge terminated.               */
    AKIRA_BATT_NOT_CHARGING,  /**< External power, not charging (fault, temp, ...). */
} akira_batt_state_t;

/**
 * @brief Battery snapshot maintained by a background poll.
 *
 * Reading it costs no I2C traffic, so UI code may call
 * akira_pm_battery_snapshot() every frame.
 */
typedef struct {
    bool     valid;          /**< pct / voltage / current come from a real gauge reading. */
    bool     online;         /**< External power present.                                 */
    akira_batt_state_t state;
    uint8_t  pct;            /**< Display SoC 0-100 %, smoothed (see power_manager.c).    */
    int32_t  voltage_mv;
    int32_t  current_ma;     /**< Charge(+) / discharge(-).                               */
    int32_t  temperature_c;  /**< INT32_MIN if unavailable.                               */
    uint32_t seq;            /**< Bumps whenever anything above changes.                  */
} akira_battery_snapshot_t;

/** @name Battery listener event bits
 * @{ */
#define AKIRA_BATT_EVT_PLUGGED   (1U << 0) /**< External power connected.           */
#define AKIRA_BATT_EVT_UNPLUGGED (1U << 1) /**< External power removed.             */
#define AKIRA_BATT_EVT_FULL      (1U << 2) /**< Charger terminated: battery full.   */
#define AKIRA_BATT_EVT_LEVEL     (1U << 3) /**< Display % changed.                  */
#define AKIRA_BATT_EVT_LOW       (1U << 4) /**< Reached LOW threshold, discharging. */
#define AKIRA_BATT_EVT_CRITICAL  (1U << 5) /**< Reached CRITICAL threshold.         */
#define AKIRA_BATT_EVT_STATE     (1U << 6) /**< state / online / validity changed.  */
/** @} */

/**
 * @brief Battery listener.  Runs on the system workqueue — keep it short
 *        (post to your own queue, don't draw).
 */
typedef void (*akira_battery_cb_t)(uint32_t events,
                                   const akira_battery_snapshot_t *snap,
                                   void *user);

/** @brief Copy the latest cached battery snapshot (no I2C). */
void akira_pm_battery_snapshot(akira_battery_snapshot_t *out);

/** @brief Re-poll the battery now (e.g. on a VBUS attach interrupt). */
void akira_pm_battery_poll_now(void);

/**
 * @brief Register a listener for battery events.
 * @return 0 on success, -ENOMEM if the listener table is full.
 */
int akira_pm_battery_register_cb(akira_battery_cb_t cb, void *user);

/**
 * @brief Override the monitor's readings for UI testing.
 * @param pct   0-100, or negative to stop simulating.
 * @param state Simulated charge state (online is derived from it).
 * @return 0, or -ENOTSUP when built without CONFIG_AKIRA_BATTERY_SIM.
 */
int akira_pm_battery_simulate(int pct, akira_batt_state_t state);

/**
 * @brief Enable or disable automatic low-power idle management.
 * @return 0 always.
 */
int akira_pm_enable_low_power_mode(bool enable);

/**
 * @brief Notify blank/wake. Idles or wakes RF/BT; callers must not touch them directly.
 * @param entering true = blanking, false = waking.
 */
void akira_pm_notify_blank(bool entering);

/**
 * @brief Register a power policy for a named app / container.
 * @return 0 on success, -ENOMEM if the policy table is full.
 */
int akira_pm_set_policy(const char *name, akira_power_policy_t policy);

/** @brief Return the most performance-demanding policy across all registered apps. */
akira_power_policy_t akira_pm_get_aggregate_policy(void);

/* ---------- insomnia (ref-counted keep-awake, Flipper-style) ---------- */

/**
 * @brief Acquire a keep-awake lock. Deep sleep allowed only at holder count 0.
 * @param reason Copied into a fixed buffer (truncated past 31 chars).
 * @param max_hold_ms Auto-expiry; 0 = never. Reaped on expiry so a leak can't pin awake forever.
 * @return Handle >= 0, or -ENOMEM if the table is full.
 */
int akira_pm_insomnia_enter(const char *reason, uint32_t max_hold_ms);

/**
 * @brief Release a keep-awake lock. Handle is generation-tagged (ABA-safe).
 * @return 0, or -EINVAL if stale/bad/already-free.
 */
int akira_pm_insomnia_exit(int handle);

/** @brief Extend a lock's expiry deadline. @return 0, or -EINVAL if stale/bad. */
int akira_pm_insomnia_renew(int handle);

/**
 * @brief Count live holders; reaps + logs expired ones. Hot path (shell loop).
 * @return Live holder count (deep sleep gated when > 0).
 */
int akira_pm_insomnia_count(void);

/** @brief Log all live holders (reason + age). @return Live count. */
int akira_pm_insomnia_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_POWER_MANAGER_H */
