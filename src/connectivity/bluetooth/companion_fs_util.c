/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "companion_fs_util.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

bool comp_fs_path_ok(const char *path)
{
    if (!path) {
        return false;
    }
    size_t n = strlen(path);
    if (n < 4 || n >= COMP_FS_PATH_MAX || strncmp(path, COMP_FS_ROOT, 4) != 0) {
        return false;
    }
    if (path[4] != '\0' && path[4] != '/') {
        return false;
    }
    for (const char *p = path; *p; p++) {
        if (*p == '"' || *p == '\\') {
            return false;
        }
    }
    for (const char *p = path + 4; *p;) {
        const char *seg = p + 1;               /* p is always '/' here */
        const char *end = strchr(seg, '/');
        size_t len = end ? (size_t)(end - seg) : strlen(seg);
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            return false;
        }
        p = end ? end : seg + len;
    }
    return true;
}

bool comp_fs_path_mutable_ok(const char *path)
{
    return comp_fs_path_ok(path) && strlen(path) > 5;
}

int comp_fs_tmp_path(const char *path, char *out, size_t out_len)
{
    int n = snprintf(out, out_len, "%s.tmp", path);
    return (n > 0 && (size_t)n < out_len) ? 0 : -ENAMETOOLONG;
}

int comp_fs_fmt_entry(char *out, size_t out_len, const char *name, bool is_dir, size_t size)
{
    int n = snprintf(out, out_len, "{\"name\":\"%s\",\"type\":\"%s\",\"size\":%zu}",
                     name, is_dir ? "dir" : "file", size);
    return (n > 0 && (size_t)n < out_len) ? n : -1;
}
