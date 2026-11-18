/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_uart_api.h
 * @brief Full-duplex UART API for WASM applications
 *
 * IRQ-driven RX ring buffer, non-blocking uart_read().
 * UART0 (zephyr_console / shell) is reserved, as is any UART the system uses
 * (console, shell, Bluetooth HCI, mcumgr, or one carrying a device node).
 * WASM apps access secondary UARTs (port_id 0 = first non-console UART).
 * A port belongs to the app that opened it and is closed, with its line
 * settings restored, when that app ends.
 * @stability stable
 * @since 1.4
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
 * @param baud_rate Desired baud rate, 1200..3000000 (e.g. 115200).
 * @return Handle index ≥0, or negative errno on failure: -EINVAL (bad port
 *         or baud), -EACCES (the system uses this UART), -EBUSY (already
 *         open), -ENODEV (no such UART).
 */
int akira_native_uart_open(wasm_exec_env_t exec_env, int32_t port_id, int32_t baud_rate);

/**
 * @brief Write bytes to an open UART.
 * @param handle Handle from uart_open().
 * @param buf Pointer to data in WASM linear memory.
 * @param len Number of bytes to write.
 * @return Bytes written, or negative errno. May be fewer than @p len: one
 *         call sends at most 4096 bytes and about 250 ms of wire time at the
 *         port's baud, and stops early when the app is being stopped.
 *         Callers loop until everything is sent.
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
 * @return 0, -EINVAL (unknown handle), -EBADF (not open) or -EPERM (opened
 *         by another app).
 */
int akira_native_uart_close(wasm_exec_env_t exec_env, int32_t handle);

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_UART_API_H */
