/*
 * Copyright (c) 2026 PenEngineering S.R.L
* SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_STORAGE_API_H
#define AKIRA_STORAGE_API_H

/**
 * @file akira_storage_api.h
 * @brief WASM sandboxed file-storage native API.
 *
 * Each WASM app gets an isolated directory:
 *   <best_mount>/apps/<app_name>/     (SD card preferred, then LittleFS, then RAM)
 *
 * All paths passed by WASM apps are relative and resolved within that sandbox.
 * Paths containing ".." are rejected with -EACCES.
 *
 * Required capabilities:
 *   storage_open  (read flags) → AKIRA_CAP_STORAGE_READ
 *   storage_open  (write/append flags) → AKIRA_CAP_STORAGE_WRITE
 *   storage_write / storage_delete → AKIRA_CAP_STORAGE_WRITE
 *   storage_read  / storage_list  → AKIRA_CAP_STORAGE_READ
 */

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

/* ── open() flag constants ─────────────────────────────────────────────── */
#define STORAGE_O_READ    0   /**< Read-only, file must exist */
#define STORAGE_O_WRITE   1   /**< Write + create/truncate */
#define STORAGE_O_APPEND  2   /**< Write + create + append (no truncate) */
#define STORAGE_O_RDWR    3   /**< Read + write + create/truncate */

#ifdef CONFIG_AKIRA_WASM_STORAGE

/**
 * @brief Open a file in the calling app's sandbox.
 *
 * @param exec_env  WAMR execution environment (caller identity).
 * @param path      Relative path (e.g. "log.txt", "sub/data.bin").
 * @param flags     STORAGE_O_READ / STORAGE_O_WRITE / STORAGE_O_APPEND / STORAGE_O_RDWR
 * @return Non-negative file descriptor on success, negative errno on error.
 */
int akira_native_storage_open(wasm_exec_env_t exec_env,
                               const char *path, int32_t flags);

/**
 * @brief Read bytes from an open file.
 *
 * @param exec_env WAMR execution environment.
 * @param fd       File descriptor from storage_open().
 * @param buf      Buffer to read into (WASM linear memory pointer).
 * @param len      Number of bytes to read.
 * @return Bytes read (0 = EOF), negative errno on error.
 */
int akira_native_storage_read(wasm_exec_env_t exec_env,
                               int32_t fd, void *buf, int32_t len);

/**
 * @brief Write bytes to an open file.
 *
 * @param exec_env WAMR execution environment.
 * @param fd       File descriptor from storage_open().
 * @param buf      Data to write (WASM linear memory pointer).
 * @param len      Number of bytes to write.
 * @return Bytes written on success, negative errno on error.
 */
int akira_native_storage_write(wasm_exec_env_t exec_env,
                                int32_t fd, const void *buf, int32_t len);

/**
 * @brief Close an open file descriptor.
 *
 * @param exec_env WAMR execution environment.
 * @param fd       File descriptor to close.
 */
void akira_native_storage_close(wasm_exec_env_t exec_env, int32_t fd);

/**
 * @brief Delete a file from the calling app's sandbox.
 *
 * @param exec_env WAMR execution environment.
 * @param path     Relative path to the file.
 * @return 0 on success, negative errno on error.
 */
int akira_native_storage_delete(wasm_exec_env_t exec_env, const char *path);

/**
 * @brief List files in the calling app's sandbox directory.
 *
 * Returns a newline-separated list of filenames, NUL-terminated.
 * Directory entries are listed with a trailing '/'.
 *
 * @param exec_env WAMR execution environment.
 * @param path     Relative subdirectory path, or "" for sandbox root.
 * @param buf      Output buffer (WASM linear memory pointer).
 * @param len      Buffer size.
 * @return Total bytes written (including NUL), negative errno on error.
 */
int akira_native_storage_list(wasm_exec_env_t exec_env,
                               const char *path, void *buf, int32_t len);

#endif /* CONFIG_AKIRA_WASM_STORAGE */

#endif /* AKIRA_STORAGE_API_H */
