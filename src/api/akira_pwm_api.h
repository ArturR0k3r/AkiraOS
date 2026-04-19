/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_pwm_api.h
 * @brief PWM channel control API for WASM applications
 *
 * Channel-indexed interface. WASM passes a logical channel number,
 * frequency in Hz, and duty cycle as a percentage (0–100).
 * Period and pulse width are derived internally.
 */

#ifndef AKIRA_PWM_API_H
#define AKIRA_PWM_API_H

#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_AKIRA_WASM_RUNTIME

/**
 * @brief Set a PWM channel's frequency and duty cycle.
 *
 * period_ns = 1,000,000,000 / freq_hz
 * pulse_ns  = period_ns * duty_pct / 100
 *
 * @param channel  Logical PWM channel index (0-based; maps to pwm0, pwm1 …).
 * @param freq_hz  Frequency in Hertz (1–10,000,000).
 * @param duty_pct Duty cycle percentage (0–100).
 * @return 0 on success, negative Zephyr errno on failure.
 */
int akira_native_pwm_set(wasm_exec_env_t exec_env,
                          int32_t channel, int32_t freq_hz, int32_t duty_pct);

/**
 * @brief Disable a PWM channel (output held low).
 * @param channel Logical PWM channel index.
 * @return 0 on success, negative Zephyr errno on failure.
 */
int akira_native_pwm_disable(wasm_exec_env_t exec_env, int32_t channel);

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_PWM_API_H */
