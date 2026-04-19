/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_uart_api.h
 * @brief Full-duplex UART API for WASM applications
 *
 * IRQ-driven RX ring buffer, non-blocking uart_read().
 * UART0 (zephyr_console / shell) is reserved.
 * WASM apps access secondary UARTs (port_id 0 = first non-console UART).
 */

#ifndef AKIRA_UART_API_H
#define AKIRA_UART_API_H

#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initialise UART API structures (called once at system init). */
int akira_uart_api_init(void);

#ifdef CONFIG_AKIRA_WASM_RUNTIME

/**
 * @brief Open a UART port and start the IRQ-driven RX handler.
 * @param port_id 0-indexed secondary UART (0 = UART1, 1 = UART2).
 * @param baud_rate Desired baud rate (e.g. 115200).
 * @return Handle index ≥0, or negative errno on failure.
 */
int akira_native_uart_open(wasm_exec_env_t exec_env, int32_t port_id, int32_t baud_rate);

/**
 * @brief Write bytes to an open UART.
 * @param handle Handle from uart_open().
 * @param buf Pointer to data in WASM linear memory.
 * @param len Number of bytes to write.
 * @return Bytes written, or negative errno.
 */
int akira_native_uart_write(wasm_exec_env_t exec_env, int32_t handle,
                             const uint8_t *buf, uint32_t len);

/**
 * @brief Non-blocking read from RX ring buffer.
 * Returns 0 immediately if no data is available.
 * @param handle Handle from uart_open().
 * @param buf Destination buffer in WASM linear memory.
 * @param max_len Maximum bytes to copy.
 * @return Bytes copied (0 = no data), or negative errno.
 */
int akira_native_uart_read(wasm_exec_env_t exec_env, int32_t handle,
                            uint8_t *buf, uint32_t max_len);

/**
 * @brief Close a UART handle, unregister IRQ callback and flush RX buffer.
 * @return 0, or -EINVAL for unknown handle.
 */
int akira_native_uart_close(wasm_exec_env_t exec_env, int32_t handle);

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_UART_API_H */
