/**
 * @file akira_sensor_api.h
 * @brief Generic sensor read API for WASM exports.
 *
 * Uses Zephyr's sensor subsystem directly. Channel IDs are Zephyr
 * enum sensor_channel integers — no chip-specific types here.
 * @stability stable
 * @since 1.4
 */

#ifndef AKIRA_SENSOR_API_H
#define AKIRA_SENSOR_API_H

#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

/**
 * @brief Read one sensor channel from any available device.
 *
 * Iterates all static DT devices, finds the first one that is ready and
 * answers the given Zephyr sensor channel, and returns its value.
 *
 * @param channel  Zephyr enum sensor_channel value, below SENSOR_CHAN_ALL
 *                 (ALL and driver-private channels are refused).
 * @param out      Pointer to store the result (float). For a vector channel
 *                 (e.g. SENSOR_CHAN_ACCEL_XYZ) this is the first component.
 * @return 0 on success, -EINVAL for a refused channel, -ENOTSUP if no device
 *         answers, or a negative errno.
 */
int akira_sensor_read(int channel, float *out);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/**
 * @brief WASM native export — sensor_read(channel) -> i32 * 1000.
 *
 * @param exec_env  WAMR execution environment (capability checked).
 * @param channel   Zephyr enum sensor_channel integer.
 * @return Value scaled ×1000 as int32 (saturated to INT32_MIN+1..INT32_MAX),
 *         or INT32_MIN on error.
 */
int akira_native_sensor_read(wasm_exec_env_t exec_env, int32_t channel);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#endif /* AKIRA_SENSOR_API_H */