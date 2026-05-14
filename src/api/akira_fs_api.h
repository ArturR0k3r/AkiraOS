/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_fs_api.h
 * @brief POSIX-like filesystem WASM native API (Track B).
 *
 * Provides seek/tell/stat/mkdir/readdir in addition to the basic
 * open/read/write/close/unlink surface.  Apps are jailed to their
 * sandbox directory.  Requires AKIRA_CAP_FS_READ / AKIRA_CAP_FS_WRITE.
 *
 * Gate: CONFIG_AKIRA_WASM_FS=y
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_FS_API_H
#define AKIRA_FS_API_H

#include <wasm_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* open flags (bit-compatible with storage_api for WASM apps) */
#define AKIRA_FS_O_READ     0x00
#define AKIRA_FS_O_WRITE    0x01
#define AKIRA_FS_O_APPEND   0x02
#define AKIRA_FS_O_RDWR     0x04
#define AKIRA_FS_O_CREATE   0x08
#define AKIRA_FS_O_TRUNC    0x10

/* seek whence */
#define AKIRA_FS_SEEK_SET 0
#define AKIRA_FS_SEEK_CUR 1
#define AKIRA_FS_SEEK_END 2

/**
 * @brief Packed stat struct exchanged with WASM sandbox.
 * All fields little-endian.
 */
typedef struct __attribute__((packed)) {
    uint32_t size;       /**< File size in bytes (0 for dirs) */
    uint8_t  type;       /**< 0 = file, 1 = directory */
    uint8_t  _pad[3];
    uint64_t mtime_ms;   /**< Modification time (ms since epoch, 0 if unknown) */
} akira_wasm_stat_t;

/* ── Exported native functions ──────────────────────────────────────────── */

int  akira_native_fs_open(wasm_exec_env_t exec_env,
                          const char *path, int32_t flags);
int  akira_native_fs_close(wasm_exec_env_t exec_env, int32_t fd);
int  akira_native_fs_read(wasm_exec_env_t exec_env,
                          int32_t fd, void *buf, uint32_t len);
int  akira_native_fs_write(wasm_exec_env_t exec_env,
                           int32_t fd, const void *buf, uint32_t len);
int  akira_native_fs_seek(wasm_exec_env_t exec_env,
                          int32_t fd, int32_t offset, int32_t whence);
int  akira_native_fs_tell(wasm_exec_env_t exec_env, int32_t fd);
int  akira_native_fs_stat(wasm_exec_env_t exec_env,
                          const char *path, akira_wasm_stat_t *out);
int  akira_native_fs_unlink(wasm_exec_env_t exec_env, const char *path);
int  akira_native_fs_mkdir(wasm_exec_env_t exec_env, const char *path);
int  akira_native_fs_readdir(wasm_exec_env_t exec_env,
                             const char *path, char *out_buf, uint32_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_FS_API_H */
