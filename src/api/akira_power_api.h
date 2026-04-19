/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_power_api.h
 * @brief WASM-facing power / battery API declarations.
 *
 * Read-only functions (battery level, mode query) require AKIRA_CAP_POWER_READ.
 * Control functions (set mode, configure wake sources) require AKIRA_CAP_POWER_CTRL
 * and must be enabled at build time with CONFIG_AKIRA_WASM_POWER_CONTROL=y.
 */

#ifndef AKIRA_POWER_API_H
#define AKIRA_POWER_API_H

#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Read-only API  (AKIRA_CAP_POWER_READ)
 * ---------------------------------------------------------------------- */

/**
 * @brief Return the current system power mode.
 *
 * WASM signature: ()i
 * @return akira_power_mode_t cast to int32, or -EACCES if denied.
 */
int akira_native_power_get_mode(wasm_exec_env_t exec_env);

/**
 * @brief Read battery state of charge.
 *
 * WASM signature: ()i
 * @return 0-100 percent, -ENODEV if no battery hardware, -EACCES if denied.
 */
int akira_native_power_get_battery_level(wasm_exec_env_t exec_env);

/**
 * @brief Read full battery status into caller-provided buffer.
 *
 * The buffer receives a packed structure:
 *   [0]   uint8  level_percent
 *   [1]   uint8  flags  (bit0=charging, bit1=low_battery)
 *   [2-3] pad
 *   [4-7] int32  voltage_mv
 *   [8-11] int32 current_ma
 *
 * WASM signature: (*~)i  (buf ptr + len, returns 0 or negative errno)
 */
int akira_native_power_get_battery_status(wasm_exec_env_t exec_env,
                                          uint8_t *buf, uint32_t buf_len);

/* -------------------------------------------------------------------------
 * Control API  (AKIRA_CAP_POWER_CTRL, CONFIG_AKIRA_WASM_POWER_CONTROL=y)
 * ---------------------------------------------------------------------- */

/**
 * @brief Request a power mode transition.
 *
 * WASM signature: (i)i
 * @param mode  0=ACTIVE, 1=IDLE, 2=LIGHT_SLEEP, 3=DEEP_SLEEP, 4=HIBERNATE
 * @return 0 on success, -ENOTSUP if the mode is compilation-disabled,
 *         -EACCES if denied.
 */
int akira_native_power_set_mode(wasm_exec_env_t exec_env, int mode);

/**
 * @brief Register a GPIO wake source.
 *
 * WASM signature: (ii)i
 * @param pin   GPIO pin number.
 * @param edge  0=low, 1=high, 2=any.
 * @return 0 on success.
 */
int akira_native_power_wake_on_gpio(wasm_exec_env_t exec_env,
                                    uint32_t pin, int edge);

/**
 * @brief Register a timer wake source.
 *
 * WASM signature: (i)i
 * @param ms Wake delay in milliseconds (>0).
 * @return 0 on success, -EINVAL if ms == 0.
 */
int akira_native_power_wake_on_timer(wasm_exec_env_t exec_env, uint32_t ms);

/**
 * @brief Enable or disable automatic low-power idle management.
 *
 * WASM signature: (i)i
 * @param enable 1 to enable, 0 to disable.
 * @return 0 always.
 */
int akira_native_power_set_low_power(wasm_exec_env_t exec_env, int enable);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_POWER_API_H */
