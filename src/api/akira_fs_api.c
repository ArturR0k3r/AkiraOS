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
 * readdir).  Apps are jailed to the same per-app directory as the storage
 * API, "<mount>/apps/<name>/" (fs_manager picks the mount: "/SD:" when a card
 * is present, else "/lfs") — any ".." in the path is rejected with -EACCES.
 * The jail directory is created on the first file or directory the app
 * creates; before that it lists as empty. It can only be listed or stat'ed.
 *
 * Containment follows akira_storage_api.c: per-app and all-apps open-file
 * caps, a per-file size quota, a free-space reserve on the volume, 4 KB
 * filesystem steps inside native critical spans, and every fd closed when its
 * app ends (fs_release).
 *
 * Gate: CONFIG_AKIRA_WASM_FS=y
 * Capabilities: AKIRA_CAP_FS_READ (bit 26), AKIRA_CAP_FS_WRITE (bit 27)
 */

#ifdef CONFIG_AKIRA_WASM_FS

#define AKIRA_WASM_SANDBOX_HELPERS
#include "akira_fs_api.h"
#include "akira_storage_api.h" /* sandbox rules shared with the storage API */
#include <akira_app_containment.h>
#include <runtime/security.h>
#include <runtime/akira_runtime.h>
#include <storage/fs_manager.h>
#include <lib/mem_helper.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>

#ifndef CONFIG_AKIRA_WASM_FS_MAX_FDS
#define CONFIG_AKIRA_WASM_FS_MAX_FDS 8
#endif

#ifndef CONFIG_AKIRA_WASM_FS_MAX_FILES_PER_APP
#define CONFIG_AKIRA_WASM_FS_MAX_FILES_PER_APP 2
#endif

#ifndef CONFIG_AKIRA_WASM_FS_QUOTA_BYTES
#define CONFIG_AKIRA_WASM_FS_QUOTA_BYTES (256 * 1024)
#endif

/* Bytes per filesystem call: the filesystem lock is held one step at a time
 * and a stop is honoured between steps. Whole requests are still served. */
#define FS_IO_STEP 4096

/* Directory entries one fs_readdir() call looks at. */
#define FS_READDIR_MAX 256

#define AKIRA_FS_O_ALL (AKIRA_FS_O_WRITE | AKIRA_FS_O_APPEND | AKIRA_FS_O_RDWR | \
                        AKIRA_FS_O_CREATE | AKIRA_FS_O_TRUNC)

atomic_t akira_sandbox_files_open __weak;

/* ── FD table ───────────────────────────────────────────────────────────── */

typedef struct {
    bool               open;
    bool               busy;      /* claimed by a call, which runs unlocked */
    bool               readable;
    bool               writable;
    bool               append;
    int8_t             owner;     /* app slot (valid while open) */
    uint32_t           size;      /* file size as far as this fd has seen */
    char               vol[8];    /* mount point, for the free-space check */
    struct fs_file_t   zfile;
} fs_fd_t;

static fs_fd_t AKIRA_BULK_BSS s_fds[CONFIG_AKIRA_WASM_FS_MAX_FDS];
static struct k_spinlock s_fd_lock;
static struct akira_sandbox_room s_room;

/* Reserve a free entry for @p slot, claimed (busy) until fd_unclaim(). */
static int fd_reserve(int slot)
{
    int owned = 0;
    int free_i = -1;
    int ret = -EMFILE;
    k_spinlock_key_t key = k_spin_lock(&s_fd_lock);

    for (int i = 0; i < CONFIG_AKIRA_WASM_FS_MAX_FDS; i++) {
        if (!s_fds[i].open) {
            if (free_i < 0) {
                free_i = i;
            }
        } else if (s_fds[i].owner == slot) {
            owned++;
        }
    }
    if (owned < CONFIG_AKIRA_WASM_FS_MAX_FILES_PER_APP && free_i >= 0 &&
        akira_sandbox_file_get()) {
        fs_fd_t *h = &s_fds[free_i];

        h->open     = true;
        h->busy     = true;
        h->readable = false;
        h->writable = false;
        h->append   = false;
        h->owner    = (int8_t)slot;
        h->size     = 0;
        h->vol[0]   = '\0';
        fs_file_t_init(&h->zfile);
        ret = free_i;
    }
    k_spin_unlock(&s_fd_lock, key);
    return ret;
}

/* Claim @p fd for one call by its owner. Another app's fd is -EBADF. */
static fs_fd_t *fd_claim(int slot, int32_t fd, int *err)
{
    fs_fd_t *h = NULL;
    k_spinlock_key_t key;

    *err = -EBADF;
    if (fd < 0 || fd >= CONFIG_AKIRA_WASM_FS_MAX_FDS) {
        return NULL;
    }
    key = k_spin_lock(&s_fd_lock);
    if (s_fds[fd].open && s_fds[fd].owner == slot) {
        if (s_fds[fd].busy) {
            *err = -EBUSY;
        } else {
            s_fds[fd].busy = true;
            h = &s_fds[fd];
        }
    }
    k_spin_unlock(&s_fd_lock, key);
    return h;
}

static void fd_unclaim(fs_fd_t *h)
{
    k_spinlock_key_t key = k_spin_lock(&s_fd_lock);

    h->busy = false;
    k_spin_unlock(&s_fd_lock, key);
}

/* The file is closed (or never opened): give the entry back. */
static void fd_free(fs_fd_t *h)
{
    k_spinlock_key_t key = k_spin_lock(&s_fd_lock);

    h->open  = false;
    h->busy  = false;
    h->owner = 0;
    k_spin_unlock(&s_fd_lock, key);
    akira_sandbox_file_put();
}

/* ── Sandbox path resolver ──────────────────────────────────────────────── */

/* Path work is kept off the app thread's native stack (only
 * CONFIG_AKIRA_WASM_STACK_GUARD_SIZE is left when a native runs, and the
 * filesystem needs most of it): one block per call, from PSRAM on boards
 * that have it (akira_malloc_app_buffer: an app can make this fail). */
struct fs_scratch {
    app_storage_ctx_t ctx; /* ctx.storage_path is the jail directory */
    char              app[AKIRA_SANDBOX_NAME_MAX];
    char              rel[AKIRA_SANDBOX_REL_MAX + 1];
    char              path[sizeof(((app_storage_ctx_t *)0)->storage_path) +
                           AKIRA_SANDBOX_REL_MAX + 2];
    struct fs_dirent  ent;
};

/**
 * sc->path = "<mount>/apps/<app>[/<rel>]", the storage API's directory for
 * the app. No filesystem access. The jail directory itself (empty or "."
 * path) only when @p root_ok: it sits next to whatever else lives in
 * "<mount>/apps".
 */
static int resolve_path(wasm_exec_env_t exec_env, const char *rel,
                        bool root_ok, struct fs_scratch *sc)
{
    int n = akira_sandbox_rel(rel, sc->rel);

    if (n < 0) {
        LOG_WRN_RATELIMIT("FS: path refused (%d)", n);
        return n;
    }
    if (n == 0 && !root_ok) {
        return -EINVAL;
    }

    /* Jail directory = the runtime's name for the calling app. No shared
     * fallback directory: an app that cannot be named gets no files. The name
     * comes from the app's manifest, so it must not step out of the jail. */
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst ||
        akira_runtime_get_name_for_module_inst(inst, sc->app, sizeof(sc->app)) < 0 ||
        !akira_sandbox_name_ok(sc->app)) {
        LOG_WRN_RATELIMIT("FS: no usable app name for the caller");
        return -EACCES;
    }

    /* No filesystem access: fs_manager only picks the mount. */
    if (fs_manager_alloc_app_storage(sc->app, 0, &sc->ctx) < 0) {
        return -ENODEV;
    }

    int len = (n == 0)
        ? snprintf(sc->path, sizeof(sc->path), "%s", sc->ctx.storage_path)
        : snprintf(sc->path, sizeof(sc->path), "%s/%s", sc->ctx.storage_path, sc->rel);
    if (len < 0 || (size_t)len >= sizeof(sc->path)) {
        return -ENAMETOOLONG;
    }
    return 0;
}

static bool is_jail_root(const struct fs_scratch *sc)
{
    return sc->rel[0] == '\0';
}

/* sc->path doesn't exist yet: may the app create it? Its parent must exist
 * (the jail itself is created here, on the first entry), and the volume
 * must keep its reserve: a file or directory costs filesystem blocks too
 * (two on LittleFS). Failures are quiet (akira_sandbox_parent_ok()) and
 * come before the room check, so they don't use up its budget. Call inside
 * a critical span. */
static int new_entry_ok(struct fs_scratch *sc)
{
    int ret = 0;

    if (strchr(sc->rel, '/') != NULL) {
        /* Parent inside the jail: if it exists, so does the jail. */
        ret = akira_sandbox_parent_ok(sc->path, &sc->ent);
    }
    if (ret == 0) {
        ret = akira_sandbox_room(&s_room, sc->path, 0);
    }
    if (ret == 0) {
        ret = fs_manager_mkdir(sc->ctx.storage_path);
    }
    return ret;
}

/* Scratch block for a path call; NULL with *err set on failure. */
static struct fs_scratch *scratch_get(int *err)
{
    struct fs_scratch *sc = akira_malloc_app_buffer(sizeof(*sc));

    *err = sc ? 0 : -ENOMEM;
    return sc;
}

/* ── fs_open ─────────────────────────────────────────────────────────────── */

int akira_native_fs_open(wasm_exec_env_t exec_env,
                         const char *path, int32_t flags)
{
    /* Anything that can create, truncate or write needs the write cap. */
    uint64_t cap = (flags & AKIRA_FS_O_ALL) ? AKIRA_CAP_FS_WRITE
                                            : AKIRA_CAP_FS_READ;
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, cap, -EPERM);
    if (!path || (flags & ~AKIRA_FS_O_ALL)) {
        return -EINVAL;
    }

    /* Always a valid access mode: AKIRA_FS_O_READ is 0, and a file opened
     * with no mode, or written when opened read-only, trips filesystem
     * asserts. */
    fs_mode_t zflags;
    if (flags & AKIRA_FS_O_RDWR) {
        zflags = FS_O_RDWR;
    } else if (flags & (AKIRA_FS_O_WRITE | AKIRA_FS_O_APPEND | AKIRA_FS_O_TRUNC)) {
        zflags = FS_O_WRITE;
    } else {
        zflags = FS_O_READ;
    }
    if (flags & AKIRA_FS_O_APPEND) { zflags |= FS_O_APPEND; }
    if (flags & AKIRA_FS_O_CREATE) { zflags |= FS_O_CREATE; }
    if (flags & AKIRA_FS_O_TRUNC)  { zflags |= FS_O_TRUNC;  }
    bool writing = (zflags & FS_O_WRITE) != 0;

    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0) {
        return -EACCES;
    }

    int ret;
    struct fs_scratch *sc = scratch_get(&ret);
    if (!sc) {
        return ret;
    }
    ret = resolve_path(exec_env, path, false, sc);
    if (ret < 0) {
        akira_free_buffer(sc);
        return ret;
    }

    int fd = fd_reserve(slot);
    if (fd < 0) {
        akira_free_buffer(sc);
        LOG_WRN_RATELIMIT("fs_open: open limit reached (per app or for all apps)");
        return -EMFILE;
    }
    fs_fd_t *h = &s_fds[fd];

    akira_native_critical_enter(exec_env);
    ret = fs_stat(sc->path, &sc->ent);
    if (ret == 0) {
        ret = (sc->ent.type == FS_DIR_ENTRY_DIR) ? -EISDIR : 0;
    } else if (ret == -ENOENT && (zflags & FS_O_CREATE)) {
        ret = new_entry_ok(sc);
    }
    if (ret == 0) {
        ret = fs_open(&h->zfile, sc->path, zflags);
    }
    if (ret == 0) {
        off_t end = 0;

        if (!(zflags & FS_O_TRUNC) && fs_seek(&h->zfile, 0, FS_SEEK_END) == 0) {
            end = fs_tell(&h->zfile);
            (void)fs_seek(&h->zfile, 0, FS_SEEK_SET);
        }
        h->size     = (end > 0) ? (uint32_t)MIN((uint64_t)end, UINT32_MAX) : 0;
        h->readable = (zflags & FS_O_READ) != 0;
        h->writable = writing;
        h->append   = (zflags & FS_O_APPEND) != 0;
        akira_sandbox_vol(sc->path, h->vol);
    }
    akira_native_critical_exit(exec_env);

    akira_free_buffer(sc);
    if (ret < 0) {
        /* Never opened: nothing to close. */
        fd_free(h);
        LOG_DBG("fs_open failed: %d", ret);
        return ret;
    }
    fd_unclaim(h);
    return fd;
}

/* ── fs_close ────────────────────────────────────────────────────────────── */

int akira_native_fs_close(wasm_exec_env_t exec_env, int32_t fd)
{
    /* Either capability closes: a write-only app must close what it wrote. */
    if (!akira_security_check_exec(exec_env, AKIRA_CAP_FS_READ) &&
        !akira_security_check_exec(exec_env, AKIRA_CAP_FS_WRITE)) {
        return -EPERM;
    }

    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0) {
        return -EACCES;
    }

    int err;
    fs_fd_t *h = fd_claim(slot, fd, &err);
    if (!h) {
        return err;
    }

    akira_native_critical_enter(exec_env);
    int ret = fs_close(&h->zfile);
    akira_native_critical_exit(exec_env);
    fd_free(h);
    return ret;
}

/* ── fs_read ─────────────────────────────────────────────────────────────── */

int akira_native_fs_read(wasm_exec_env_t exec_env,
                         int32_t fd, void *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_READ, -EPERM);
    if (!buf || len == 0) {
        return -EINVAL;
    }

    /* Validate the app buffer before claiming anything. */
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst || !wasm_runtime_validate_native_addr(inst, buf, len)) {
        return -EFAULT;
    }
    /* The count is returned as an int. */
    len = MIN(len, (uint32_t)INT32_MAX);

    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0) {
        return -EACCES;
    }

    int err;
    fs_fd_t *h = fd_claim(slot, fd, &err);
    if (!h) {
        return err;
    }
    if (!h->readable) {
        fd_unclaim(h);
        return -EBADF;
    }

    /* The whole request, one step per filesystem call. */
    int n = 0;

    while ((uint32_t)n < len) {
        size_t step = MIN((size_t)(len - (uint32_t)n), (size_t)FS_IO_STEP);

        akira_native_critical_enter(exec_env);
        ssize_t got = fs_read(&h->zfile, (uint8_t *)buf + n, step);
        akira_native_critical_exit(exec_env);
        if (got < 0) {
            if (n == 0) {
                n = (int)got;
            }
            break;
        }
        n += (int)got;
        if ((size_t)got < step || akira_runtime_stop_requested(exec_env)) {
            break;
        }
    }

    fd_unclaim(h);
    return n;
}

/* ── fs_write ────────────────────────────────────────────────────────────── */

int akira_native_fs_write(wasm_exec_env_t exec_env,
                          int32_t fd, const void *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_WRITE, -EPERM);
    if (!buf || len == 0) {
        return -EINVAL;
    }

    /* Validate the app buffer before claiming anything. */
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst || !wasm_runtime_validate_native_addr(inst, (void *)buf, len)) {
        return -EFAULT;
    }
    len = MIN(len, (uint32_t)INT32_MAX);

    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0) {
        return -EACCES;
    }

    int err;
    fs_fd_t *h = fd_claim(slot, fd, &err);
    if (!h) {
        return err;
    }
    if (!h->writable) {
        fd_unclaim(h);
        return -EBADF;
    }

    /* No file grows past the quota, and the volume keeps its reserve. */
    int ret = 0;
    uint64_t pos;

    akira_native_critical_enter(exec_env);
    if (h->append) {
        pos = h->size;
    } else {
        off_t at = fs_tell(&h->zfile);

        ret = (at < 0) ? (int)at : 0;
        pos = (at < 0) ? 0 : (uint64_t)at;
    }
    if (ret == 0 && pos + len > (uint64_t)CONFIG_AKIRA_WASM_FS_QUOTA_BYTES) {
        ret = -ENOSPC;
    }
    if (ret == 0 && pos + len > h->size) {
        ret = akira_sandbox_room(&s_room, h->vol, pos + len - h->size);
    }
    akira_native_critical_exit(exec_env);

    if (ret < 0) {
        fd_unclaim(h);
        return ret;
    }

    int n = 0;

    while ((uint32_t)n < len) {
        size_t step = MIN((size_t)(len - (uint32_t)n), (size_t)FS_IO_STEP);

        akira_native_critical_enter(exec_env);
        ssize_t put = fs_write(&h->zfile, (const uint8_t *)buf + n, step);
        akira_native_critical_exit(exec_env);
        if (put < 0) {
            if (n == 0) {
                n = (int)put;
            }
            break;
        }
        n += (int)put;
        if ((size_t)put < step || akira_runtime_stop_requested(exec_env)) {
            break;
        }
    }
    if (n > 0 && pos + (uint64_t)n > h->size) {
        h->size = (uint32_t)(pos + (uint64_t)n);
    }

    fd_unclaim(h);
    return n;
}

/* ── fs_seek ─────────────────────────────────────────────────────────────── */

int akira_native_fs_seek(wasm_exec_env_t exec_env,
                         int32_t fd, int32_t offset, int32_t whence)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_READ, -EPERM);

    int zwhence;
    switch (whence) {
    case AKIRA_FS_SEEK_SET: zwhence = FS_SEEK_SET; break;
    case AKIRA_FS_SEEK_CUR: zwhence = FS_SEEK_CUR; break;
    case AKIRA_FS_SEEK_END: zwhence = FS_SEEK_END; break;
    default:                return -EINVAL;
    }

    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0) {
        return -EACCES;
    }

    int err;
    fs_fd_t *h = fd_claim(slot, fd, &err);
    if (!h) {
        return err;
    }

    akira_native_critical_enter(exec_env);

    /* Where the seek lands. Past the end of a writable file it counts as
     * growth (FAT extends the file on such a seek): quota and reserve apply. */
    int ret = 0;
    int64_t base = 0;
    if (zwhence == FS_SEEK_CUR) {
        off_t at = fs_tell(&h->zfile);

        ret = (at < 0) ? (int)at : 0;
        base = at;
    } else if (zwhence == FS_SEEK_END) {
        ret = fs_seek(&h->zfile, 0, FS_SEEK_END);
        if (ret == 0) {
            off_t end = fs_tell(&h->zfile);

            ret = (end < 0) ? (int)end : 0;
            base = end;
            if (end > 0 && (uint64_t)end > h->size) {
                h->size = (uint32_t)MIN((uint64_t)end, UINT32_MAX);
            }
        }
    }
    int64_t target = base + offset;

    if (ret == 0 && target < 0) {
        ret = -EINVAL;
    }
    if (ret == 0 && h->writable && (uint64_t)target > h->size) {
        if ((uint64_t)target > (uint64_t)CONFIG_AKIRA_WASM_FS_QUOTA_BYTES) {
            ret = -ENOSPC;
        } else {
            ret = akira_sandbox_room(&s_room, h->vol, (uint64_t)target - h->size);
        }
    }
    if (ret == 0) {
        ret = fs_seek(&h->zfile, (off_t)target, FS_SEEK_SET);
    }

    akira_native_critical_exit(exec_env);

    fd_unclaim(h);
    return ret;
}

/* ── fs_tell ─────────────────────────────────────────────────────────────── */

int akira_native_fs_tell(wasm_exec_env_t exec_env, int32_t fd)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_READ, -EPERM);

    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0) {
        return -EACCES;
    }

    int err;
    fs_fd_t *h = fd_claim(slot, fd, &err);
    if (!h) {
        return err;
    }

    akira_native_critical_enter(exec_env);
    off_t pos = fs_tell(&h->zfile);
    akira_native_critical_exit(exec_env);

    fd_unclaim(h);
    return (pos > INT32_MAX) ? -EOVERFLOW : (int)pos;
}

/* ── fs_stat ─────────────────────────────────────────────────────────────── */

int akira_native_fs_stat(wasm_exec_env_t exec_env,
                         const char *path, akira_wasm_stat_t *out)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_READ, -EPERM);
    if (!path || !out) {
        return -EINVAL;
    }

    /* "$*": WAMR checked one byte of @p out, not the whole struct. */
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst || !wasm_runtime_validate_native_addr(inst, out, sizeof(*out))) {
        return -EFAULT;
    }

    int ret;
    struct fs_scratch *sc = scratch_get(&ret);
    if (!sc) {
        return ret;
    }
    ret = resolve_path(exec_env, path, true, sc);
    if (ret == 0) {
        akira_native_critical_enter(exec_env);
        ret = fs_stat(sc->path, &sc->ent);
        akira_native_critical_exit(exec_env);
        if (ret == -ENOENT && is_jail_root(sc)) {
            /* Not created yet: an empty directory to the app. */
            memset(&sc->ent, 0, sizeof(sc->ent));
            sc->ent.type = FS_DIR_ENTRY_DIR;
            ret = 0;
        }
    }
    if (ret == 0) {
        akira_wasm_stat_t st = {0};

        st.size     = (uint32_t)sc->ent.size;
        st.type     = (sc->ent.type == FS_DIR_ENTRY_DIR) ? 1u : 0u;
        st.mtime_ms = 0; /* Zephyr fs_dirent has no mtime */
        memcpy(out, &st, sizeof(st));
    }
    akira_free_buffer(sc);
    return ret;
}

/* ── fs_unlink ───────────────────────────────────────────────────────────── */

int akira_native_fs_unlink(wasm_exec_env_t exec_env, const char *path)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_WRITE, -EPERM);
    if (!path) {
        return -EINVAL;
    }

    int ret;
    struct fs_scratch *sc = scratch_get(&ret);
    if (!sc) {
        return ret;
    }
    /* Never the jail directory itself. */
    ret = resolve_path(exec_env, path, false, sc);
    if (ret == 0) {
        akira_native_critical_enter(exec_env);
        ret = akira_sandbox_unlink_ok(sc->path, &sc->ent);
        if (ret == 0) {
            ret = fs_unlink(sc->path);
        }
        akira_native_critical_exit(exec_env);
    }
    akira_free_buffer(sc);
    return ret;
}

/* ── fs_mkdir ────────────────────────────────────────────────────────────── */

int akira_native_fs_mkdir(wasm_exec_env_t exec_env, const char *path)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_WRITE, -EPERM);
    if (!path) {
        return -EINVAL;
    }

    int ret;
    struct fs_scratch *sc = scratch_get(&ret);
    if (!sc) {
        return ret;
    }
    ret = resolve_path(exec_env, path, false, sc);
    if (ret == 0) {
        akira_native_critical_enter(exec_env);
        ret = fs_stat(sc->path, &sc->ent);
        if (ret == 0) {
            ret = -EEXIST;
        } else if (ret == -ENOENT) {
            ret = new_entry_ok(sc);
        }
        if (ret == 0) {
            ret = fs_mkdir(sc->path);
        }
        akira_native_critical_exit(exec_env);
    }
    akira_free_buffer(sc);
    return ret;
}

/* ── fs_readdir ──────────────────────────────────────────────────────────── */

/**
 * Enumerate directory entries into out_buf as newline-separated names.
 * Returns number of entries written, or negative errno.
 */
int akira_native_fs_readdir(wasm_exec_env_t exec_env,
                            const char *path, char *out_buf, uint32_t out_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_FS_READ, -EPERM);
    if (!path || !out_buf || out_len == 0) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst || !wasm_runtime_validate_native_addr(inst, out_buf, out_len)) {
        return -EFAULT;
    }

    int ret;
    struct fs_scratch *sc = scratch_get(&ret);
    if (!sc) {
        return ret;
    }
    ret = resolve_path(exec_env, path, true, sc);
    if (ret < 0) {
        akira_free_buffer(sc);
        return ret;
    }

    struct fs_dir_t dir;
    int count = 0;
    uint32_t pos = 0;

    fs_dir_t_init(&dir);

    akira_native_critical_enter(exec_env);
    ret = akira_sandbox_dir_ok(sc->path, &sc->ent);
    if (ret == 0) {
        ret = fs_opendir(&dir, sc->path);
    }
    if (ret == 0) {
        /* Bounded: a directory with many entries is listed in part. */
        for (int seen = 0; seen < FS_READDIR_MAX && pos < out_len - 1; seen++) {
            if (fs_readdir(&dir, &sc->ent) < 0 || sc->ent.name[0] == '\0') {
                break;
            }
            size_t name_len = akira_sandbox_strnlen(sc->ent.name, sizeof(sc->ent.name));
            if ((uint64_t)pos + name_len + 1 >= out_len) {
                break; /* Buffer full */
            }
            memcpy(out_buf + pos, sc->ent.name, name_len);
            pos += (uint32_t)name_len;
            out_buf[pos++] = '\n';
            count++;
        }
        out_buf[pos] = '\0';
        (void)fs_closedir(&dir);
    } else if (ret == -ENOENT && is_jail_root(sc)) {
        /* Not created yet: an empty directory to the app. */
        out_buf[0] = '\0';
        ret = 0;
    }
    akira_native_critical_exit(exec_env);

    akira_free_buffer(sc);
    return (ret < 0) ? ret : count;
}

/* ── App teardown ────────────────────────────────────────────────────────── */

/* Close what the app left open. Runs after the app thread is gone (or on it,
 * after main() returned). */
static void fs_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);

    for (int i = 0; i < CONFIG_AKIRA_WASM_FS_MAX_FDS; i++) {
        fs_fd_t *h = &s_fds[i];
        bool close_it = false;
        k_spinlock_key_t key = k_spin_lock(&s_fd_lock);

        /* "busy" here means the thread was stopped between two file steps,
         * outside any critical span: it held no filesystem lock, so closing
         * is safe. A thread that died inside a file call (in a critical
         * span) never gets here: its slot is parked instead. */
        if (h->open && h->owner == slot) {
            h->busy = true;
            close_it = true;
        }
        k_spin_unlock(&s_fd_lock, key);

        if (close_it) {
            (void)fs_close(&h->zfile);
            fd_free(h);
            LOG_DBG("fs_release: closed fd=%d (slot %d)", i, slot);
        }
    }
}

AKIRA_APP_RESOURCE_DEFINE(fs_api, 50, NULL, fs_release);

#endif /* CONFIG_AKIRA_WASM_FS */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_FS))
#include <akira_native_registry.h>

/* fs.*: POSIX-like sandbox filesystem (seek/tell/stat/mkdir/readdir) */
static const NativeSymbol akira_fs_natives[] = {
    {"fs_open",    (void *)akira_native_fs_open,    "($i)i",   NULL},
    {"fs_close",   (void *)akira_native_fs_close,   "(i)i",    NULL},
    {"fs_read",    (void *)akira_native_fs_read,    "(i*~)i",  NULL},
    {"fs_write",   (void *)akira_native_fs_write,   "(i*~)i",  NULL},
    {"fs_seek",    (void *)akira_native_fs_seek,    "(iii)i",  NULL},
    {"fs_tell",    (void *)akira_native_fs_tell,    "(i)i",    NULL},
    {"fs_stat",    (void *)akira_native_fs_stat,    "($*)i",   NULL},
    {"fs_unlink",  (void *)akira_native_fs_unlink,  "($)i",    NULL},
    {"fs_mkdir",   (void *)akira_native_fs_mkdir,   "($)i",    NULL},
    {"fs_readdir", (void *)akira_native_fs_readdir, "($*~)i",  NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_fs_api, "env", akira_fs_natives);
#endif
