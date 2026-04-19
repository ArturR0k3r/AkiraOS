/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_i2c_api.h
 * @brief Raw I2C register read/write API for WASM applications
 *
 * Stateless API — no handles. WASM passes bus_id + device_addr + reg_addr
 * per call. Buses are lazily resolved from the device tree on first use.
 * Supports standard 7-bit addressing and register-based burst I/O.
 */

#ifndef AKIRA_I2C_API_H
#define AKIRA_I2C_API_H

#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_AKIRA_WASM_RUNTIME

/**
 * @brief Write bytes to an I2C device register.
 * @param bus_id   0 = i2c0, 1 = i2c1.
 * @param dev_addr 7-bit I2C device address (≤ 0x7F).
 * @param reg_addr Register address to start writing at.
 * @param buf      Data buffer in WASM linear memory.
 * @param len      Number of bytes to write (max 256).
 * @return 0 on success, negative Zephyr errno on failure.
 */
int akira_native_i2c_write_reg(wasm_exec_env_t exec_env,
                                int32_t bus_id, int32_t dev_addr, int32_t reg_addr,
                                const uint8_t *buf, uint32_t len);

/**
 * @brief Read bytes from an I2C device register.
 * @param bus_id   0 = i2c0, 1 = i2c1.
 * @param dev_addr 7-bit I2C device address (≤ 0x7F).
 * @param reg_addr Register address to start reading from.
 * @param buf      Destination buffer in WASM linear memory.
 * @param len      Number of bytes to read (max 256).
 * @return Bytes read on success, negative Zephyr errno on failure.
 */
int akira_native_i2c_read_reg(wasm_exec_env_t exec_env,
                               int32_t bus_id, int32_t dev_addr, int32_t reg_addr,
                               uint8_t *buf, uint32_t len);

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_I2C_API_H */
