/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_NFC_API_H
#define AKIRA_NFC_API_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <runtime/security.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum single-call NFC read/write length. */
#define AKIRA_NFC_MAX_XFER_LEN 256

/**
 * @brief Read the tag's 8-byte UID.
 * @return 0 on success, negative error code (-ENODEV if no NFC device registered).
 * @stability experimental
 * @since 1.5
 */
int akira_nfc_uid(uint8_t uid[8]);

/**
 * @brief Read from tag user memory.
 * @param addr Byte address within user memory.
 * @param buf  Destination buffer.
 * @param len  Number of bytes to read (max AKIRA_NFC_MAX_XFER_LEN).
 * @return 0 on success, negative error code.
 */
int akira_nfc_read(uint16_t addr, uint8_t *buf, size_t len);

/**
 * @brief Write to tag user memory.
 * @param addr Byte address within user memory.
 * @param buf  Source buffer.
 * @param len  Number of bytes to write (max AKIRA_NFC_MAX_XFER_LEN).
 * @return 0 on success, negative error code.
 */
int akira_nfc_write(uint16_t addr, const uint8_t *buf, size_t len);

/**
 * @brief Check RF field presence.
 * @return 1 if present, 0 if absent, negative error code on failure.
 */
int akira_nfc_field_present(void);

/**
 * @brief Enable/disable Fast Transfer Mode (FTM) mailbox.
 * @param enable true to enable, false to disable.
 * @param wdg    Watchdog setting 0-7: duration = 2^(wdg-1) x 30ms, 0 = infinite.
 * @return 0 on success, negative error code.
 */
int akira_nfc_mb_enable(bool enable, uint8_t wdg);

/**
 * @brief Put a message in the FTM mailbox for an RF reader to read.
 * @param buf Source buffer.
 * @param len Message length (1-256).
 * @return 0 on success, negative error code.
 */
int akira_nfc_mb_put(const uint8_t *buf, size_t len);

/**
 * @brief Get the message an RF reader put in the FTM mailbox.
 * @param buf Destination buffer.
 * @param cap Destination buffer capacity.
 * @return Number of bytes read (>=0) on success, negative error code.
 */
int akira_nfc_mb_get(uint8_t *buf, size_t cap);

/**
 * @brief Query FTM mailbox control/status bits and current message length.
 * @param ctrl_out    Set to the raw MB_CTRL_Dyn register value.
 * @param msg_len_out Set to the current message length.
 * @return 0 on success, negative error code.
 */
int akira_nfc_mb_status(uint8_t *ctrl_out, size_t *msg_len_out);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/**
 * @brief WASM native: nfc_uid(uid_out[8]) -> 0/errno
 * Capability: AKIRA_CAP_NFC ("nfc")
 */
int akira_native_nfc_uid(wasm_exec_env_t exec_env, uint8_t *uid_out);

/**
 * @brief WASM native: nfc_read(addr, buf, len) -> 0/errno
 * Capability: AKIRA_CAP_NFC ("nfc")
 */
int akira_native_nfc_read(wasm_exec_env_t exec_env, int32_t addr, uint8_t *buf, uint32_t len);

/**
 * @brief WASM native: nfc_write(addr, buf, len) -> 0/errno
 * Capability: AKIRA_CAP_NFC ("nfc")
 */
int akira_native_nfc_write(wasm_exec_env_t exec_env, int32_t addr, const uint8_t *buf, uint32_t len);

/**
 * @brief WASM native: nfc_field_present() -> 1/0/errno
 * Capability: AKIRA_CAP_NFC ("nfc")
 */
int akira_native_nfc_field_present(wasm_exec_env_t exec_env);

/**
 * @brief WASM native: nfc_mb_enable(enable, wdg) -> 0/errno
 * Capability: AKIRA_CAP_NFC ("nfc")
 */
int akira_native_nfc_mb_enable(wasm_exec_env_t exec_env, int32_t enable, uint8_t wdg);

/**
 * @brief WASM native: nfc_mb_put(buf, len) -> 0/errno
 * Capability: AKIRA_CAP_NFC ("nfc")
 */
int akira_native_nfc_mb_put(wasm_exec_env_t exec_env, uint8_t *buf, uint32_t len);

/**
 * @brief WASM native: nfc_mb_get(buf, cap) -> len/errno
 * Capability: AKIRA_CAP_NFC ("nfc")
 */
int akira_native_nfc_mb_get(wasm_exec_env_t exec_env, uint8_t *buf, uint32_t cap);

/**
 * @brief WASM native: nfc_mb_status(ctrl_out, msg_len_out) -> 0/errno
 * Capability: AKIRA_CAP_NFC ("nfc")
 */
int akira_native_nfc_mb_status(wasm_exec_env_t exec_env, uint8_t *ctrl_out, uint32_t *msg_len_out);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_NFC_API_H */
