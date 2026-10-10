/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Pure helpers for the BLE companion file ops (no Zephyr fs calls, host-testable).
 */
#ifndef COMPANION_FS_UTIL_H_
#define COMPANION_FS_UTIL_H_

#include <stdbool.h>
#include <stddef.h>

#define COMP_FS_ROOT     "/SD:"
#define COMP_FS_PATH_MAX 80

/** Path is under /SD:, shorter than COMP_FS_PATH_MAX, no ".." segment, no '"' or '\'. */
bool comp_fs_path_ok(const char *path);
/** comp_fs_path_ok() and not the volume root. Use for write, delete, rename, mkdir. */
bool comp_fs_path_mutable_ok(const char *path);
/** "<path>.tmp" into out. 0, or -ENAMETOOLONG. */
int comp_fs_tmp_path(const char *path, char *out, size_t out_len);
/** One JSON list entry. Bytes written, or -1 if it does not fit in out_len. */
int comp_fs_fmt_entry(char *out, size_t out_len, const char *name, bool is_dir, size_t size);

#endif
