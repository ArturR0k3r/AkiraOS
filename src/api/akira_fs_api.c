/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_fs_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_fs_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_fs_api.c
 * @brief POSIX-like filesystem WASM native API.
 *
 * Provides a richer surface than akira_storage_api (adds seek/tell/stat/mkdir/
 * readdir).  Apps are jailed to /akira/apps/<name>/ — any ".." component
 * in the path is rejected with -EACCES.
 *
 * Gate: CONFIG_AKIRA_WASM_FS=y
 * Capabilities: AKIRA_CAP_FS_READ (bit 26), AKIRA_CAP_FS_WRITE (bit 27)
 */

#ifdef CONFIG_AKIRA_WASM_FS

#include "akira_fs_api.h"
#include <runtime/security.h>
#include <runtime/akira_runtime.h>
#include <storage/fs_manager.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>

#ifndef CONFIG_AKIRA_WASM_FS_MAX_FDS
#define CONFIG_AKIRA_WASM_FS_MAX_FDS 8
#endif

/* ── FD table ───────────────────────────────────────────────────────────── */

typedef struct {
    bool               open;
    wasm_module_inst_t owner;
    struct fs_file_t   zfile;
} fs_fd_t;

static fs_fd_t       s_fds[CONFIG_AKIRA_WASM_FS_MAX_FDS];
static K_MUTEX_DEFINE(s_fds_mutex);

static int fd_alloc(wasm_module_inst_t owner)
{
    for (int i = 0; i < CONFIG_AKIRA_WASM_FS_MAX_FDS; i++) {
        if (!s_fds[i].open) {
            s_fds[i].open  = true;
            s_fds[i].owner = owner;
            fs_file_t_init(&s_fds[i].zfile);
            return i;
        }
    }
    return -EMFILE;
}

static fs_fd_t *fd_get(wasm_module_inst_t owner, int32_t fd)
{
    if (fd < 0 || fd >= CONFIG_AKIRA_WASM_FS_MAX_FDS) {
        return NULL;
    }
    fs_fd_t *h = &s_fds[fd];
    if (!h->open || h->owner != owner) {
        return NULL;
    }
    return h;
}

static void fd_release(fs_fd_t *h)
{
    h->open  = false;
    h->owner = NULL;
}

/* ── Sandbox path resolver ──────────────────────────────────────────────── */

#define SANDBOX_PATH_MAX 256
#define SANDBOX_BASE     "/akira/apps"

static int resolve_path(wasm_exec_env_t exec_env,
                        const char *rel, char *out, size_t out_len)
{
    if (!rel || !out) {
        return -EINVAL;
    }
    /* Reject path traversal */
    if (strstr(rel, "..")) {
        LOG_WRN("FS: path traversal blocked: '%s'", rel);
        return -EACCES;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    const char *app_name    = wasm_runtime_get_module_name(inst);
    if (!app_name || app_name[0] == '\0') {
        app_name = "unknown";
    }

    int n = snprintf(out, out_len, "%s/%s/%s", SANDBOX_BASE, app_name, rel);
    if (n < 0 || (size_t)n >= out_len) {
        return -ENAMETOOLONG;
    }
    return 0;
}

/* ── fs_open ─────────────────────────────────────────────────────────────── */

int akira_native_fs_open(wasm_exec_env_t exec_env,
                         const char *path, int32_t flags)
{
    uint32_t cap = (flags & AKIRA_FS_O_WRITE) ? AKIRA_CAP_FS_WRITE
                                              : AKIRA_CAP_FS_READ;
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, cap, -EACCES);
    if (!path) {
        return -EINVAL;
    }

    char full[SANDBOX_PATH_MAX];
    int ret = resolve_path(exec_env, path, full, sizeof(full));
    if (ret < 0) {
        return ret;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);

    k_mutex_lock(&s_fds_mutex, K_FOREVER);
    int fd = fd_alloc(inst);
    if (fd < 0) {
        k_mutex_unlock(&s_fds_mutex);
        return -EMFILE;
    }
    k_mutex_unlock(&s_fds_mutex);

    fs_mode_t zflags = 0;
    if (flags & AKIRA_FS_O_WRITE)  { zflags |= FS_O_WRITE; }
    if (flags & AKIRA_FS_O_READ)   { zflags |= FS_O_READ;  }
    if (flags & AKIRA_FS_O_RDWR)   { zflags |= FS_O_RDWR;  }
    if (flags & AKIRA_FS_O_APPEND) { zflags |= FS_O_APPEND; }
    if (flags & AKIRA_FS_O_CREATE) { zflags |= FS_O_CREATE; }
    if (flags & AKIRA_FS_O_TRUNC)  { zflags |= FS_O_TRUNC;  }

    ret = fs_open(&s_fds[fd].zfile, full, zflags);
    if (ret < 0) {
        k_mutex_lock(&s_fds_mutex, K_FOREVER);
        fd_release(&s_fds[fd]);
        k_mutex_unlock(&s_fds_mutex);
        LOG_DBG("fs_open '%s' failed: %d", full, ret);
        return ret;
    }
    return fd;
}

/* ── fs_close ────────────────────────────────────────────────────────────── */

int akira_native_fs_close(wasm_exec_env_t exec_env, int32_t fd)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_READ, -EACCES);
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);

    k_mutex_lock(&s_fds_mutex, K_FOREVER);
    fs_fd_t *h = fd_get(inst, fd);
    if (!h) {
        k_mutex_unlock(&s_fds_mutex);
        return -EBADF;
    }
    int ret = fs_close(&h->zfile);
    fd_release(h);
    k_mutex_unlock(&s_fds_mutex);
    return ret;
}

/* ── fs_read ─────────────────────────────────────────────────────────────── */

int akira_native_fs_read(wasm_exec_env_t exec_env,
                         int32_t fd, void *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_READ, -EACCES);
    if (!buf || len == 0) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_native_addr(inst, buf, len)) {
        return -EFAULT;
    }

    k_mutex_lock(&s_fds_mutex, K_FOREVER);
    fs_fd_t *h = fd_get(inst, fd);
    if (!h) {
        k_mutex_unlock(&s_fds_mutex);
        return -EBADF;
    }
    ssize_t n = fs_read(&h->zfile, buf, (size_t)len);
    k_mutex_unlock(&s_fds_mutex);
    return (int)n;
}

/* ── fs_write ────────────────────────────────────────────────────────────── */

int akira_native_fs_write(wasm_exec_env_t exec_env,
                          int32_t fd, const void *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_WRITE, -EACCES);
    if (!buf || len == 0) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_native_addr(inst, (void *)buf, len)) {
        return -EFAULT;
    }

    k_mutex_lock(&s_fds_mutex, K_FOREVER);
    fs_fd_t *h = fd_get(inst, fd);
    if (!h) {
        k_mutex_unlock(&s_fds_mutex);
        return -EBADF;
    }
    ssize_t n = fs_write(&h->zfile, buf, (size_t)len);
    k_mutex_unlock(&s_fds_mutex);
    return (int)n;
}

/* ── fs_seek ─────────────────────────────────────────────────────────────── */

int akira_native_fs_seek(wasm_exec_env_t exec_env,
                         int32_t fd, int32_t offset, int32_t whence)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_READ, -EACCES);
    if (whence < AKIRA_FS_SEEK_SET || whence > AKIRA_FS_SEEK_END) {
        return -EINVAL;
    }

    int zwhence;
    switch (whence) {
    case AKIRA_FS_SEEK_SET: zwhence = FS_SEEK_SET; break;
    case AKIRA_FS_SEEK_CUR: zwhence = FS_SEEK_CUR; break;
    case AKIRA_FS_SEEK_END: zwhence = FS_SEEK_END; break;
    default:                return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);

    k_mutex_lock(&s_fds_mutex, K_FOREVER);
    fs_fd_t *h = fd_get(inst, fd);
    if (!h) {
        k_mutex_unlock(&s_fds_mutex);
        return -EBADF;
    }
    int ret = fs_seek(&h->zfile, (off_t)offset, zwhence);
    k_mutex_unlock(&s_fds_mutex);
    return ret;
}

/* ── fs_tell ─────────────────────────────────────────────────────────────── */

int akira_native_fs_tell(wasm_exec_env_t exec_env, int32_t fd)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_READ, -EACCES);
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);

    k_mutex_lock(&s_fds_mutex, K_FOREVER);
    fs_fd_t *h = fd_get(inst, fd);
    if (!h) {
        k_mutex_unlock(&s_fds_mutex);
        return -EBADF;
    }
    off_t pos = fs_tell(&h->zfile);
    k_mutex_unlock(&s_fds_mutex);
    return (int)pos;
}

/* ── fs_stat ─────────────────────────────────────────────────────────────── */

int akira_native_fs_stat(wasm_exec_env_t exec_env,
                         const char *path, akira_wasm_stat_t *out)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_READ, -EACCES);
    if (!path || !out) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_native_addr(inst, out, sizeof(*out))) {
        return -EFAULT;
    }

    char full[SANDBOX_PATH_MAX];
    int ret = resolve_path(exec_env, path, full, sizeof(full));
    if (ret < 0) {
        return ret;
    }

    struct fs_dirent entry;
    ret = fs_stat(full, &entry);
    if (ret < 0) {
        return ret;
    }

    memset(out, 0, sizeof(*out));
    out->size    = (uint32_t)entry.size;
    out->type    = (entry.type == FS_DIR_ENTRY_DIR) ? 1u : 0u;
    out->mtime_ms = 0; /* Zephyr fs_dirent has no mtime */
    return 0;
}

/* ── fs_unlink ───────────────────────────────────────────────────────────── */

int akira_native_fs_unlink(wasm_exec_env_t exec_env, const char *path)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_WRITE, -EACCES);
    if (!path) {
        return -EINVAL;
    }

    char full[SANDBOX_PATH_MAX];
    int ret = resolve_path(exec_env, path, full, sizeof(full));
    if (ret < 0) {
        return ret;
    }
    return fs_unlink(full);
}

/* ── fs_mkdir ────────────────────────────────────────────────────────────── */

int akira_native_fs_mkdir(wasm_exec_env_t exec_env, const char *path)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_WRITE, -EACCES);
    if (!path) {
        return -EINVAL;
    }

    char full[SANDBOX_PATH_MAX];
    int ret = resolve_path(exec_env, path, full, sizeof(full));
    if (ret < 0) {
        return ret;
    }
    return fs_mkdir(full);
}

/* ── fs_readdir ──────────────────────────────────────────────────────────── */

/**
 * Enumerate directory entries into out_buf as newline-separated names.
 * Returns number of entries written, or negative errno.
 */
int akira_native_fs_readdir(wasm_exec_env_t exec_env,
                            const char *path, char *out_buf, uint32_t out_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_READ, -EACCES);
    if (!path || !out_buf || out_len == 0) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_native_addr(inst, out_buf, out_len)) {
        return -EFAULT;
    }

    char full[SANDBOX_PATH_MAX];
    int ret = resolve_path(exec_env, path, full, sizeof(full));
    if (ret < 0) {
        return ret;
    }

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);
    ret = fs_opendir(&dir, full);
    if (ret < 0) {
        return ret;
    }

    int count = 0;
    uint32_t pos = 0;

    while (pos < out_len - 1) {
        struct fs_dirent entry;
        ret = fs_readdir(&dir, &entry);
        if (ret < 0 || entry.name[0] == '\0') {
            break;
        }
        size_t name_len = strlen(entry.name);
        if (pos + name_len + 1 >= out_len) {
            break; /* Buffer full */
        }
        memcpy(out_buf + pos, entry.name, name_len);
        pos += name_len;
        out_buf[pos++] = '\n';
        count++;
    }
    out_buf[pos] = '\0';
    (void)fs_closedir(&dir);
    return count;
}

#endif /* CONFIG_AKIRA_WASM_FS */
