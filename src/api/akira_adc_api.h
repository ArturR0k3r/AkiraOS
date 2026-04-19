/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_ADC_API_H
#define AKIRA_ADC_API_H

#include <stdint.h>
#include <runtime/security.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read raw ADC sample from a channel.
 *
 * @param channel  ADC channel index (0 .. CONFIG_AKIRA_WASM_ADC_MAX_CHANNELS-1)
 * @return Raw ADC sample value, or negative error code.
 */
int akira_adc_read(int channel);

/**
 * @brief Read ADC channel and convert to millivolts.
 *
 * Conversion uses CONFIG_AKIRA_WASM_ADC_VREF_MV and the configured resolution.
 *
 * @param channel  ADC channel index
 * @return Voltage in millivolts, or negative error code.
 */
int akira_adc_read_mv(int channel);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/**
 * @brief WASM native: adc_read(channel) -> raw sample
 * Capability: AKIRA_CAP_ADC ("adc")
 */
int akira_native_adc_read(wasm_exec_env_t exec_env, int32_t channel);

/**
 * @brief WASM native: adc_read_mv(channel) -> millivolts
 * Capability: AKIRA_CAP_ADC ("adc")
 */
int akira_native_adc_read_mv(wasm_exec_env_t exec_env, int32_t channel);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_ADC_API_H */
