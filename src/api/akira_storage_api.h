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
 * Limits: CONFIG_AKIRA_WASM_STORAGE_MAX_FILES_PER_APP open files per app (and
 * apps together always leave the OS two of the filesystem's file slots); no
 * file grows past CONFIG_AKIRA_WASM_STORAGE_QUOTA_BYTES; a write that would
 * leave less than a quarter of the volume free returns -ENOSPC. An app's
 * files are closed when it ends, however it ends.
 *
 * Required capabilities:
 *   storage_open  (read flags) → AKIRA_CAP_STORAGE_READ
 *   storage_open  (write/append flags) → AKIRA_CAP_STORAGE_WRITE
 *   storage_write / storage_delete → AKIRA_CAP_STORAGE_WRITE
 *   storage_read  / storage_list  → AKIRA_CAP_STORAGE_READ
 *   storage_close → either
 * @stability stable
 * @since 1.3
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

/* ── Sandbox rules shared by the storage and fs APIs ─────────────────────
 * Internal: only akira_storage_api.c and akira_fs_api.c define
 * AKIRA_WASM_SANDBOX_HELPERS before including this header. */
#ifdef AKIRA_WASM_SANDBOX_HELPERS

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

/** Longest path an app may pass (relative to its sandbox). */
#define AKIRA_SANDBOX_REL_MAX 192

/** Longest app name the runtime keeps (akira_managed_app_t.name). */
#define AKIRA_SANDBOX_NAME_MAX 32

/* strnlen() is POSIX, not C99: not every libc in the tree declares it. */
static inline size_t akira_sandbox_strnlen(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

/* The filesystems' open-file pools are shared with the OS (installer, app
 * loader, HTTP...): all apps together, through both APIs, leave it two. */
#if defined(CONFIG_FILE_SYSTEM_LITTLEFS) && defined(CONFIG_FAT_FILESYSTEM_ELM)
#define AKIRA_SANDBOX_FS_SLOTS MIN(CONFIG_FS_LITTLEFS_NUM_FILES, CONFIG_FS_FATFS_NUM_FILES)
#elif defined(CONFIG_FILE_SYSTEM_LITTLEFS)
#define AKIRA_SANDBOX_FS_SLOTS CONFIG_FS_LITTLEFS_NUM_FILES
#elif defined(CONFIG_FAT_FILESYSTEM_ELM)
#define AKIRA_SANDBOX_FS_SLOTS CONFIG_FS_FATFS_NUM_FILES
#else
#define AKIRA_SANDBOX_FS_SLOTS 4
#endif
#define AKIRA_SANDBOX_FILES_MAX MAX(1, AKIRA_SANDBOX_FS_SLOTS - 2)

/* A write that grows the filesystem must leave this much of the volume free
 * for the OS and installed apps: a quarter of it, 64 KB to 8 MB. */
#define AKIRA_SANDBOX_RESERVE_MIN (64ULL * 1024ULL)
#define AKIRA_SANDBOX_RESERVE_MAX (8ULL * 1024ULL * 1024ULL)

/**
 * Files all apps hold open through both APIs. Defined weak in each of the two
 * API files, so either builds alone and both share one counter.
 */
extern atomic_t akira_sandbox_files_open;

/** Take one of the apps' file slots; false if they are all in use. */
static inline bool akira_sandbox_file_get(void)
{
    atomic_val_t n;

    do {
        n = atomic_get(&akira_sandbox_files_open);
        if (n >= AKIRA_SANDBOX_FILES_MAX) {
            return false;
        }
    } while (!atomic_cas(&akira_sandbox_files_open, n, n + 1));
    return true;
}

static inline void akira_sandbox_file_put(void)
{
    atomic_val_t n;

    do {
        n = atomic_get(&akira_sandbox_files_open);
        if (n <= 0) {
            return;
        }
    } while (!atomic_cas(&akira_sandbox_files_open, n, n - 1));
}

/**
 * The sandbox directory is named after the app, and the name comes from the
 * app's own manifest: it must be one plain path component.
 */
static inline bool akira_sandbox_name_ok(const char *name)
{
    size_t n = akira_sandbox_strnlen(name, AKIRA_SANDBOX_NAME_MAX);

    if (n == 0 || n >= AKIRA_SANDBOX_NAME_MAX || strstr(name, "..") ||
        (n == 1 && name[0] == '.')) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':') {
            return false;
        }
    }
    return true;
}

/**
 * Copy an app's path into @p out (AKIRA_SANDBOX_REL_MAX + 1 bytes) in
 * canonical form: empty and "." components dropped, no leading or trailing
 * '/'. Checks run on the copy, so the app can't change the path afterwards.
 *
 * @return length of the result (0 = the sandbox directory itself);
 *         -ENAMETOOLONG, -EACCES for "..", -EINVAL for '\\', ':' or control
 *         characters.
 */
static inline int akira_sandbox_rel(const char *rel, char *out)
{
    size_t n, o = 0, i = 0;

    if (!rel) {
        return -EINVAL;
    }
    n = akira_sandbox_strnlen(rel, AKIRA_SANDBOX_REL_MAX + 1);
    if (n > AKIRA_SANDBOX_REL_MAX) {
        return -ENAMETOOLONG;
    }
    memcpy(out, rel, n);
    out[n] = '\0';
    if (strstr(out, "..")) {
        return -EACCES;
    }

    /* Compact in place: the write index never passes the read index. */
    while (i < n) {
        size_t start = i;

        while (i < n && out[i] != '/') {
            unsigned char c = (unsigned char)out[i];

            if (c < 0x20 || c == 0x7f || c == '\\' || c == ':') {
                return -EINVAL;
            }
            i++;
        }
        size_t len = i - start;

        if (len > 0 && !(len == 1 && out[start] == '.')) {
            if (o > 0) {
                out[o++] = '/';
            }
            memmove(out + o, out + start, len);
            o += len;
        }
        i++; /* the '/' */
    }
    out[o] = '\0';
    return (int)o;
}

/** Cached free space of the volume apps last wrote to. */
struct akira_sandbox_room {
    struct k_spinlock lock;
    char vol[8];
    int64_t at_ms;
    uint64_t free;
    uint64_t total;
    uint32_t block;
};

/** Mount point of an absolute path ("/lfs", "/SD:"), truncated to 7 chars. */
static inline void akira_sandbox_vol(const char *path, char vol[8])
{
    size_t i = 1;

    vol[0] = '/';
    while (i < 7 && path[i] != '\0' && path[i] != '/') {
        vol[i] = path[i];
        i++;
    }
    vol[i] = '\0';
}

/**
 * May the volume holding @p path grow by @p grow bytes? Keeps the reserve
 * free, counting two blocks of filesystem overhead. fs_statvfs() can walk
 * the whole filesystem, so its answer is reused for a second and reduced by
 * what was granted since. Takes the filesystem lock: call it inside a
 * critical span.
 *
 * @return 0, -ENOSPC, or the fs_statvfs() error.
 */
static inline int akira_sandbox_room(struct akira_sandbox_room *r,
                                     const char *path, uint64_t grow)
{
    char vol[8];
    uint64_t free, total, need, reserve;
    uint32_t block;
    bool fresh;
    k_spinlock_key_t key;

    akira_sandbox_vol(path, vol);
    key = k_spin_lock(&r->lock);
    fresh = r->at_ms != 0 && strcmp(r->vol, vol) == 0 &&
            k_uptime_get() - r->at_ms < 1000;
    free = r->free;
    total = r->total;
    block = r->block;
    k_spin_unlock(&r->lock, key);

    if (!fresh) {
        struct fs_statvfs st;
        int ret = fs_statvfs(vol, &st);

        if (ret < 0) {
            return ret;
        }
        block = (uint32_t)st.f_frsize;
        free = (uint64_t)st.f_bfree * st.f_frsize;
        total = (uint64_t)st.f_blocks * st.f_frsize;
        key = k_spin_lock(&r->lock);
        memcpy(r->vol, vol, sizeof(r->vol));
        r->at_ms = k_uptime_get();
        r->free = free;
        r->total = total;
        r->block = block;
        k_spin_unlock(&r->lock, key);
    }

    reserve = CLAMP(total / 4, AKIRA_SANDBOX_RESERVE_MIN, AKIRA_SANDBOX_RESERVE_MAX);
    need = grow + 2ULL * block;
    if (free < reserve || free - reserve < need) {
        return -ENOSPC;
    }

    key = k_spin_lock(&r->lock);
    if (strcmp(r->vol, vol) == 0) {
        r->free = (r->free > need) ? r->free - need : 0;
    }
    k_spin_unlock(&r->lock, key);
    return 0;
}

/*
 * Quiet pre-checks. Zephyr's fs layer logs every failed open, opendir,
 * unlink and mkdir at error level, so an app passing missing or wrong paths
 * in a loop would flood the log. fs_stat() is quiet on -ENOENT, so the
 * natives check with it first and fail the same way without the log line.
 * They take the filesystem lock: call them inside a critical span.
 */

/** The parent of @p path (modified and restored) exists and is a directory. */
static inline int akira_sandbox_parent_ok(char *path, struct fs_dirent *ent)
{
    char *slash = strrchr(path, '/');
    int ret;

    if (!slash || slash == path) {
        return 0;
    }
    *slash = '\0';
    ret = fs_stat(path, ent);
    *slash = '/';
    if (ret < 0) {
        return ret;
    }
    return (ent->type == FS_DIR_ENTRY_DIR) ? 0 : -ENOTDIR;
}

/** @p path is an existing regular file, or may be created as one. */
static inline int akira_sandbox_file_ok(char *path, struct fs_dirent *ent,
                                        bool create)
{
    int ret = fs_stat(path, ent);

    if (ret == 0) {
        return (ent->type == FS_DIR_ENTRY_DIR) ? -EISDIR : 0;
    }
    if (ret == -ENOENT && create) {
        return akira_sandbox_parent_ok(path, ent);
    }
    return ret;
}

/** @p path is a directory that can be listed. */
static inline int akira_sandbox_dir_ok(const char *path, struct fs_dirent *ent)
{
    int ret = fs_stat(path, ent);

    if (ret < 0) {
        return ret;
    }
    return (ent->type == FS_DIR_ENTRY_DIR) ? 0 : -ENOTDIR;
}

/** @p path exists and, if a directory, is empty, so unlink can succeed. */
static inline int akira_sandbox_unlink_ok(const char *path, struct fs_dirent *ent)
{
    int ret = fs_stat(path, ent);

    if (ret < 0 || ent->type != FS_DIR_ENTRY_DIR) {
        return ret;
    }

    struct fs_dir_t dir;

    fs_dir_t_init(&dir);
    ret = fs_opendir(&dir, path);
    if (ret < 0) {
        return ret;
    }
    ret = fs_readdir(&dir, ent);
    if (ret == 0 && ent->name[0] != '\0') {
        ret = -ENOTEMPTY;
    }
    (void)fs_closedir(&dir);
    return ret;
}

#endif /* AKIRA_WASM_SANDBOX_HELPERS */

#endif /* AKIRA_STORAGE_API_H */
