/*
 * Copyright (c) 2026 PenEngineering S.R.L
* SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_storage_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_storage_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_storage_api.c
 * @brief Sandboxed file-storage WASM native API.
 *
 * Each app is confined to:
 *   <best_mount>/apps/<app_name>/
 *   SD card preferred; falls back to LittleFS, then RAM.
 *
 * Path traversal ("..") is rejected with -EACCES.
 * Max CONFIG_AKIRA_WASM_STORAGE_MAX_FDS concurrent open files (default 8).
 */

#ifdef CONFIG_AKIRA_WASM_STORAGE

#include "akira_storage_api.h"
#include <runtime/security.h>
#include <runtime/akira_runtime.h>
#include <storage/fs_manager.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifndef CONFIG_AKIRA_WASM_STORAGE_MAX_FDS
#define CONFIG_AKIRA_WASM_STORAGE_MAX_FDS 8
#endif

/* --------------------------------------------------------------------------
 * File descriptor table
 * -------------------------------------------------------------------------- */

typedef struct {
    bool               open;
    wasm_module_inst_t owner;
    struct fs_file_t   zfile;
} storage_fd_t;

static storage_fd_t  s_fds[CONFIG_AKIRA_WASM_STORAGE_MAX_FDS];
static K_MUTEX_DEFINE(s_fd_mutex);

static int fd_alloc(wasm_module_inst_t owner)
{
    for (int i = 0; i < CONFIG_AKIRA_WASM_STORAGE_MAX_FDS; i++) {
        if (!s_fds[i].open) {
            s_fds[i].open  = true;
            s_fds[i].owner = owner;
            fs_file_t_init(&s_fds[i].zfile);
            return i;
        }
    }
    return -EMFILE;
}

static storage_fd_t *fd_get(wasm_module_inst_t owner, int32_t fd)
{
    if (fd < 0 || fd >= CONFIG_AKIRA_WASM_STORAGE_MAX_FDS) {
        return NULL;
    }
    storage_fd_t *h = &s_fds[fd];
    if (!h->open || h->owner != owner) {
        return NULL;
    }
    return h;
}

static void fd_release(storage_fd_t *h)
{
    if (h) {
        fs_close(&h->zfile);
        h->open  = false;
        h->owner = NULL;
    }
}

/* --------------------------------------------------------------------------
 * Sandbox path resolution
 * -------------------------------------------------------------------------- */

/**
 * Resolve <relative> against the calling app's sandbox root.
 * Returns -EACCES if ".." traversal is detected.
 * Returns -EINVAL if app name cannot be determined.
 */
static int resolve_sandbox_path(wasm_exec_env_t exec_env,
                                 const char *relative,
                                 char *out, size_t out_len)
{
    if (!relative || !out || out_len == 0) {
        return -EINVAL;
    }

    /* Reject any "../" traversal attempt */
    if (strstr(relative, "..") != NULL) {
        LOG_WRN("storage: path traversal rejected: %s", relative);
        return -EACCES;
    }

    /* Identify calling app */
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst) {
        return -EINVAL;
    }

    char app_name[32] = {0};
    if (akira_runtime_get_name_for_module_inst(inst, app_name, sizeof(app_name)) < 0
        || app_name[0] == '\0') {
        LOG_ERR("storage: cannot identify calling app");
        return -EINVAL;
    }

    /* Choose best mount point via fs_manager */
    app_storage_ctx_t ctx;
    int ret = fs_manager_alloc_app_storage(app_name, 0, &ctx);
    if (ret < 0) {
        return ret;
    }

    /* ctx.storage_path = "<mount>/apps/<name>" — create data subdir as sandbox */
    char sandbox[sizeof(ctx.storage_path) + 8];
    snprintf(sandbox, sizeof(sandbox), "%s", ctx.storage_path);

    /* Ensure sandbox directory exists */
    (void)fs_manager_mkdir(sandbox);

    /* Build final path */
    if (relative[0] == '\0' || (relative[0] == '/' && relative[1] == '\0')) {
        /* Empty or "/" → root of sandbox */
        snprintf(out, out_len, "%s", sandbox);
    } else {
        const char *rel = (relative[0] == '/') ? relative + 1 : relative;
        snprintf(out, out_len, "%s/%s", sandbox, rel);
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * WASM native exports
 * -------------------------------------------------------------------------- */

int akira_native_storage_open(wasm_exec_env_t exec_env,
                               const char *path, int32_t flags)
{
    /* Capability check depends on access mode */
    uint32_t cap = (flags == STORAGE_O_READ) ? AKIRA_CAP_STORAGE_READ
                                              : AKIRA_CAP_STORAGE_WRITE;
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, cap, -EPERM);

    if (!path || path[0] == '\0') {
        return -EINVAL;
    }

    char full_path[512];
    int ret = resolve_sandbox_path(exec_env, path, full_path, sizeof(full_path));
    if (ret < 0) {
        return ret;
    }

    /* Map flags to Zephyr FS flags */
    fs_mode_t zflags;
    switch (flags) {
    case STORAGE_O_READ:
        zflags = FS_O_READ;
        break;
    case STORAGE_O_WRITE:
        zflags = FS_O_WRITE | FS_O_CREATE;
        break;
    case STORAGE_O_APPEND:
        zflags = FS_O_WRITE | FS_O_CREATE | FS_O_APPEND;
        break;
    case STORAGE_O_RDWR:
        zflags = FS_O_READ | FS_O_WRITE | FS_O_CREATE;
        break;
    default:
        return -EINVAL;
    }

    k_mutex_lock(&s_fd_mutex, K_FOREVER);

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    int fd = fd_alloc(inst);
    if (fd < 0) {
        k_mutex_unlock(&s_fd_mutex);
        LOG_WRN("storage: no free file descriptors");
        return -EMFILE;
    }

    /* Ensure parent directory exists */
    char parent[sizeof(full_path)];
    strncpy(parent, full_path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
        (void)fs_manager_mkdir(parent);
    }

    /* Truncate on write (not append) */
    ret = fs_open(&s_fds[fd].zfile, full_path, zflags);
    if (ret < 0) {
        LOG_DBG("storage_open(%s) failed: %d", full_path, ret);
        s_fds[fd].open = false;
        k_mutex_unlock(&s_fd_mutex);
        return ret;
    }

    if (flags == STORAGE_O_WRITE || flags == STORAGE_O_RDWR) {
        (void)fs_truncate(&s_fds[fd].zfile, 0);
        (void)fs_seek(&s_fds[fd].zfile, 0, FS_SEEK_SET);
    }

    k_mutex_unlock(&s_fd_mutex);

    LOG_DBG("storage_open fd=%d path=%s", fd, full_path);
    return fd;
}

int akira_native_storage_read(wasm_exec_env_t exec_env,
                               int32_t fd, void *buf, int32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_STORAGE_READ, -EPERM);

    if (!buf || len <= 0) {
        return -EINVAL;
    }

    k_mutex_lock(&s_fd_mutex, K_FOREVER);

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    storage_fd_t *h = fd_get(inst, fd);
    if (!h) {
        k_mutex_unlock(&s_fd_mutex);
        return -EBADF;
    }

    ssize_t n = fs_read(&h->zfile, buf, (size_t)len);
    k_mutex_unlock(&s_fd_mutex);

    return (n < 0) ? (int)n : (int)n;
}

int akira_native_storage_write(wasm_exec_env_t exec_env,
                                int32_t fd, const void *buf, int32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_STORAGE_WRITE, -EPERM);

    if (!buf || len <= 0) {
        return -EINVAL;
    }

    k_mutex_lock(&s_fd_mutex, K_FOREVER);

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    storage_fd_t *h = fd_get(inst, fd);
    if (!h) {
        k_mutex_unlock(&s_fd_mutex);
        return -EBADF;
    }

    ssize_t n = fs_write(&h->zfile, buf, (size_t)len);
    k_mutex_unlock(&s_fd_mutex);

    return (n < 0) ? (int)n : (int)n;
}

void akira_native_storage_close(wasm_exec_env_t exec_env, int32_t fd)
{
    k_mutex_lock(&s_fd_mutex, K_FOREVER);

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    storage_fd_t *h = fd_get(inst, fd);
    fd_release(h);

    k_mutex_unlock(&s_fd_mutex);
}

int akira_native_storage_delete(wasm_exec_env_t exec_env, const char *path)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_STORAGE_WRITE, -EPERM);

    if (!path || path[0] == '\0') {
        return -EINVAL;
    }

    char full_path[512];
    int ret = resolve_sandbox_path(exec_env, path, full_path, sizeof(full_path));
    if (ret < 0) {
        return ret;
    }

    ret = fs_unlink(full_path);
    if (ret < 0 && ret != -ENOENT) {
        LOG_DBG("storage_delete(%s) failed: %d", full_path, ret);
    }
    return ret;
}

int akira_native_storage_list(wasm_exec_env_t exec_env,
                               const char *path, void *buf, int32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_STORAGE_READ, -EPERM);

    if (!buf || len <= 0) {
        return -EINVAL;
    }

    char dir_path[512];
    int ret = resolve_sandbox_path(exec_env, path ? path : "", dir_path, sizeof(dir_path));
    if (ret < 0) {
        return ret;
    }

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);

    ret = fs_opendir(&dir, dir_path);
    if (ret < 0) {
        LOG_DBG("storage_list: opendir(%s) failed: %d", dir_path, ret);
        return ret;
    }

    char *out = (char *)buf;
    int   pos = 0;
    struct fs_dirent entry;

    while (fs_readdir(&dir, &entry) == 0) {
        if (entry.name[0] == '\0') {
            break; /* end of directory */
        }

        /* Skip self/parent entries */
        if (entry.name[0] == '.') {
            continue;
        }

        size_t name_len = strlen(entry.name);
        int    needed   = (int)name_len + 1; /* +1 for '\n' or ' /' '+'\n' */

        if (entry.type == FS_DIR_ENTRY_DIR) {
            needed += 1; /* trailing '/' */
        }

        if (pos + needed + 1 >= len) { /* +1 for final NUL */
            break;
        }

        memcpy(out + pos, entry.name, name_len);
        pos += (int)name_len;

        if (entry.type == FS_DIR_ENTRY_DIR) {
            out[pos++] = '/';
        }
        out[pos++] = '\n';
    }

    fs_closedir(&dir);

    /* Null-terminate */
    if (pos < len) {
        out[pos] = '\0';
    } else {
        out[len - 1] = '\0';
        pos = len;
    }

    return pos + 1; /* includes NUL */
}

#endif /* CONFIG_AKIRA_WASM_STORAGE */
