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
 * Path traversal ("..") is rejected with -EACCES. The sandbox directory
 * itself can only be listed: it sits next to installed apps' files.
 * Max CONFIG_AKIRA_WASM_STORAGE_MAX_FDS concurrent open files (default 8),
 * CONFIG_AKIRA_WASM_STORAGE_MAX_FILES_PER_APP of them per app.
 *
 * Containment: the fd table is guarded by a spinlock and never held across
 * filesystem calls; each filesystem call runs inside a native critical span
 * and moves at most STORAGE_IO_STEP bytes, so a stop never aborts a thread
 * that holds the filesystem lock. Files left open are closed when the app
 * ends (storage_release).
 */

#ifdef CONFIG_AKIRA_WASM_STORAGE

#define AKIRA_WASM_SANDBOX_HELPERS
#include "akira_storage_api.h"
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

#ifndef CONFIG_AKIRA_WASM_STORAGE_MAX_FDS
#define CONFIG_AKIRA_WASM_STORAGE_MAX_FDS 8
#endif

#ifndef CONFIG_AKIRA_WASM_STORAGE_MAX_FILES_PER_APP
#define CONFIG_AKIRA_WASM_STORAGE_MAX_FILES_PER_APP 2
#endif

#ifndef CONFIG_AKIRA_WASM_STORAGE_QUOTA_BYTES
#define CONFIG_AKIRA_WASM_STORAGE_QUOTA_BYTES (256 * 1024)
#endif

/* Bytes per filesystem call: the filesystem lock is held one step at a time
 * and a stop is honoured between steps. Whole requests are still served. */
#define STORAGE_IO_STEP 4096

/* Directory entries one storage_list() call looks at. */
#define STORAGE_LIST_MAX 256

atomic_t akira_sandbox_files_open __weak;

/* --------------------------------------------------------------------------
 * File descriptor table
 * -------------------------------------------------------------------------- */

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
} storage_fd_t;

static storage_fd_t AKIRA_BULK_BSS s_fds[CONFIG_AKIRA_WASM_STORAGE_MAX_FDS];
static struct k_spinlock s_fd_lock;
static struct akira_sandbox_room s_room;

/* Reserve a free entry for @p slot, claimed (busy) until fd_unclaim(). */
static int fd_reserve(int slot)
{
    int owned = 0;
    int free_i = -1;
    int ret = -EMFILE;
    k_spinlock_key_t key = k_spin_lock(&s_fd_lock);

    for (int i = 0; i < CONFIG_AKIRA_WASM_STORAGE_MAX_FDS; i++) {
        if (!s_fds[i].open) {
            if (free_i < 0) {
                free_i = i;
            }
        } else if (s_fds[i].owner == slot) {
            owned++;
        }
    }
    if (owned < CONFIG_AKIRA_WASM_STORAGE_MAX_FILES_PER_APP && free_i >= 0 &&
        akira_sandbox_file_get()) {
        storage_fd_t *h = &s_fds[free_i];

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
static storage_fd_t *fd_claim(int slot, int32_t fd, int *err)
{
    storage_fd_t *h = NULL;
    k_spinlock_key_t key;

    *err = -EBADF;
    if (fd < 0 || fd >= CONFIG_AKIRA_WASM_STORAGE_MAX_FDS) {
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

static void fd_unclaim(storage_fd_t *h)
{
    k_spinlock_key_t key = k_spin_lock(&s_fd_lock);

    h->busy = false;
    k_spin_unlock(&s_fd_lock, key);
}

/* The file is closed (or never opened): give the entry back. */
static void fd_free(storage_fd_t *h)
{
    k_spinlock_key_t key = k_spin_lock(&s_fd_lock);

    h->open  = false;
    h->busy  = false;
    h->owner = 0;
    k_spin_unlock(&s_fd_lock, key);
    akira_sandbox_file_put();
}

/* --------------------------------------------------------------------------
 * Sandbox path resolution
 * -------------------------------------------------------------------------- */

/* Path work is kept off the app thread's native stack (only
 * CONFIG_AKIRA_WASM_STACK_GUARD_SIZE is left when a native runs, and the
 * filesystem needs most of it): one block per call, from PSRAM on boards
 * that have it (akira_malloc_app_buffer: an app can make this fail). */
struct storage_scratch {
    app_storage_ctx_t ctx;
    char              app[AKIRA_SANDBOX_NAME_MAX];
    char              rel[AKIRA_SANDBOX_REL_MAX + 1];
    char              path[sizeof(((app_storage_ctx_t *)0)->storage_path) +
                           AKIRA_SANDBOX_REL_MAX + 2];
    struct fs_dirent  ent;
};

/**
 * Check the app's @p relative path and name into @p sc. No filesystem access.
 * The sandbox directory itself (empty path) only when @p root_ok.
 */
static int sandbox_prepare(wasm_exec_env_t exec_env, const char *relative,
                           bool root_ok, struct storage_scratch *sc)
{
    int n = akira_sandbox_rel(relative, sc->rel);

    if (n < 0) {
        LOG_WRN_RATELIMIT("storage: path refused (%d)", n);
        return n;
    }
    if (n == 0 && !root_ok) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);

    if (!inst ||
        akira_runtime_get_name_for_module_inst(inst, sc->app, sizeof(sc->app)) < 0 ||
        !akira_sandbox_name_ok(sc->app)) {
        LOG_WRN_RATELIMIT("storage: no usable app name for the caller");
        return -EACCES;
    }
    return 0;
}

/**
 * sc->path = "<mount>/apps/<app>[/<rel>]". Takes the filesystem lock
 * (fs_manager picks the mount): call inside a critical span.
 */
static int sandbox_path(struct storage_scratch *sc)
{
    int ret = fs_manager_alloc_app_storage(sc->app, 0, &sc->ctx);

    if (ret < 0) {
        return ret;
    }
    if (sc->rel[0] == '\0') {
        ret = snprintf(sc->path, sizeof(sc->path), "%s", sc->ctx.storage_path);
    } else {
        ret = snprintf(sc->path, sizeof(sc->path), "%s/%s", sc->ctx.storage_path, sc->rel);
    }
    if (ret < 0 || (size_t)ret >= sizeof(sc->path)) {
        return -ENAMETOOLONG;
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
    uint64_t cap = (flags == STORAGE_O_READ) ? AKIRA_CAP_STORAGE_READ
                                              : AKIRA_CAP_STORAGE_WRITE;
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, cap, -EPERM);

    if (!path || path[0] == '\0') {
        return -EINVAL;
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
    bool writing = (zflags & FS_O_WRITE) != 0;

    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0) {
        return -EACCES;
    }

    struct storage_scratch *sc = akira_malloc_app_buffer(sizeof(*sc));
    if (!sc) {
        return -ENOMEM;
    }

    int ret = sandbox_prepare(exec_env, path, false, sc);
    if (ret < 0) {
        akira_free_buffer(sc);
        return ret;
    }

    int fd = fd_reserve(slot);
    if (fd < 0) {
        akira_free_buffer(sc);
        LOG_WRN_RATELIMIT("storage: open limit reached (per app or for all apps)");
        return -EMFILE;
    }
    storage_fd_t *h = &s_fds[fd];

    akira_native_critical_enter(exec_env);

    ret = sandbox_path(sc);
    if (ret == 0 && writing) {
        /* Creating directories and the file needs room too. */
        ret = akira_sandbox_room(&s_room, sc->path, 0);
    }
    if (ret == 0 && writing) {
        /* Ensure the sandbox directory and the file's parent exist. */
        (void)fs_manager_mkdir(sc->ctx.storage_path);
        char *slash = strrchr(sc->rel, '/');
        if (slash) {
            size_t base = strlen(sc->ctx.storage_path);
            size_t cut = base + 1 + (size_t)(slash - sc->rel);

            sc->path[cut] = '\0';
            /* One level only: skip it if its own parent is missing. */
            if (akira_sandbox_parent_ok(sc->path, &sc->ent) == 0) {
                (void)fs_manager_mkdir(sc->path);
            }
            sc->path[cut] = '/';
        }
    }
    if (ret == 0) {
        ret = akira_sandbox_file_ok(sc->path, &sc->ent, writing);
    }
    if (ret == 0) {
        ret = fs_open(&h->zfile, sc->path, zflags);
    }
    if (ret == 0) {
        off_t end = 0;

        if (flags == STORAGE_O_WRITE || flags == STORAGE_O_RDWR) {
            /* Truncate on write (not append) */
            (void)fs_truncate(&h->zfile, 0);
            (void)fs_seek(&h->zfile, 0, FS_SEEK_SET);
        } else if (fs_seek(&h->zfile, 0, FS_SEEK_END) == 0) {
            end = fs_tell(&h->zfile);
            if (flags == STORAGE_O_READ) {
                (void)fs_seek(&h->zfile, 0, FS_SEEK_SET);
            }
        }
        h->size     = (end > 0) ? (uint32_t)MIN((uint64_t)end, UINT32_MAX) : 0;
        h->readable = (zflags & FS_O_READ) != 0;
        h->writable = writing;
        h->append   = (flags == STORAGE_O_APPEND);
        akira_sandbox_vol(sc->path, h->vol);
    }

    akira_native_critical_exit(exec_env);

    if (ret < 0) {
        LOG_DBG("storage_open failed: %d", ret);
        /* Never opened: nothing to close. */
        fd_free(h);
        akira_free_buffer(sc);
        return ret;
    }
    fd_unclaim(h);
    akira_free_buffer(sc);

    LOG_DBG("storage_open fd=%d", fd);
    return fd;
}

int akira_native_storage_read(wasm_exec_env_t exec_env,
                               int32_t fd, void *buf, int32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_STORAGE_READ, -EPERM);

    /* WAMR checked [buf, buf + len) ("*~"). */
    if (!buf || len <= 0) {
        return -EINVAL;
    }

    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0) {
        return -EACCES;
    }

    int err;
    storage_fd_t *h = fd_claim(slot, fd, &err);
    if (!h) {
        return err;
    }
    if (!h->readable) {
        fd_unclaim(h);
        return -EBADF;
    }

    /* The whole request, one step per filesystem call. */
    int n = 0;

    while (n < len) {
        size_t step = MIN((size_t)(len - n), (size_t)STORAGE_IO_STEP);

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

int akira_native_storage_write(wasm_exec_env_t exec_env,
                                int32_t fd, const void *buf, int32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_STORAGE_WRITE, -EPERM);

    /* WAMR checked [buf, buf + len) ("*~"). */
    if (!buf || len <= 0) {
        return -EINVAL;
    }

    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0) {
        return -EACCES;
    }

    int err;
    storage_fd_t *h = fd_claim(slot, fd, &err);
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
    if (ret == 0 && pos + (uint64_t)len > (uint64_t)CONFIG_AKIRA_WASM_STORAGE_QUOTA_BYTES) {
        ret = -ENOSPC;
    }
    if (ret == 0 && pos + (uint64_t)len > h->size) {
        ret = akira_sandbox_room(&s_room, h->vol, pos + (uint64_t)len - h->size);
    }
    akira_native_critical_exit(exec_env);

    if (ret < 0) {
        fd_unclaim(h);
        return ret;
    }

    int n = 0;

    while (n < len) {
        size_t step = MIN((size_t)(len - n), (size_t)STORAGE_IO_STEP);

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

void akira_native_storage_close(wasm_exec_env_t exec_env, int32_t fd)
{
    /* Either capability closes: a write-only app must be able to close what
     * it wrote, or every save would leak a descriptor. */
    if (!akira_security_check_exec(exec_env, AKIRA_CAP_STORAGE_WRITE) &&
        !akira_security_check_exec(exec_env, AKIRA_CAP_STORAGE_READ)) {
        return;
    }

    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0) {
        return;
    }

    int err;
    storage_fd_t *h = fd_claim(slot, fd, &err);
    if (!h) {
        return;
    }

    akira_native_critical_enter(exec_env);
    (void)fs_close(&h->zfile);
    akira_native_critical_exit(exec_env);
    fd_free(h);
}

int akira_native_storage_delete(wasm_exec_env_t exec_env, const char *path)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_STORAGE_WRITE, -EPERM);

    if (!path || path[0] == '\0') {
        return -EINVAL;
    }

    struct storage_scratch *sc = akira_malloc_app_buffer(sizeof(*sc));
    if (!sc) {
        return -ENOMEM;
    }

    /* Not the sandbox directory itself: an app named after another app's
     * file ("x.wasm") would otherwise delete that file. */
    int ret = sandbox_prepare(exec_env, path, false, sc);
    if (ret == 0) {
        akira_native_critical_enter(exec_env);
        ret = sandbox_path(sc);
        if (ret == 0) {
            ret = akira_sandbox_unlink_ok(sc->path, &sc->ent);
        }
        if (ret == 0) {
            ret = fs_unlink(sc->path);
        }
        akira_native_critical_exit(exec_env);
        if (ret < 0 && ret != -ENOENT) {
            LOG_DBG("storage_delete failed: %d", ret);
        }
    }

    akira_free_buffer(sc);
    return ret;
}

int akira_native_storage_list(wasm_exec_env_t exec_env,
                               const char *path, void *buf, int32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_STORAGE_READ, -EPERM);

    /* WAMR checked [buf, buf + len) ("*~"). */
    if (!buf || len <= 0) {
        return -EINVAL;
    }

    struct storage_scratch *sc = akira_malloc_app_buffer(sizeof(*sc));
    if (!sc) {
        return -ENOMEM;
    }

    int ret = sandbox_prepare(exec_env, path ? path : "", true, sc);
    if (ret < 0) {
        akira_free_buffer(sc);
        return ret;
    }

    char *out = (char *)buf;
    int   pos = 0;
    struct fs_dir_t dir;

    fs_dir_t_init(&dir);

    akira_native_critical_enter(exec_env);

    ret = sandbox_path(sc);
    if (ret == 0) {
        ret = akira_sandbox_dir_ok(sc->path, &sc->ent);
        if (ret == 0) {
            ret = fs_opendir(&dir, sc->path);
        }
        if (ret == -ENOENT && sc->rel[0] == '\0') {
            ret = 1; /* the app has no files yet: empty listing */
        }
    }
    if (ret == 0) {
        /* Bounded: a directory with many entries is listed in part. */
        for (int seen = 0; seen < STORAGE_LIST_MAX; seen++) {
            if (fs_readdir(&dir, &sc->ent) != 0 || sc->ent.name[0] == '\0') {
                break; /* end of directory */
            }

            /* Skip self/parent entries */
            if (sc->ent.name[0] == '.') {
                continue;
            }

            size_t name_len = akira_sandbox_strnlen(sc->ent.name, sizeof(sc->ent.name));
            int    needed   = (int)name_len + 1; /* +1 for '\n' */

            if (sc->ent.type == FS_DIR_ENTRY_DIR) {
                needed += 1; /* trailing '/' */
            }

            if (pos + needed + 1 >= len) { /* +1 for final NUL */
                break;
            }

            memcpy(out + pos, sc->ent.name, name_len);
            pos += (int)name_len;

            if (sc->ent.type == FS_DIR_ENTRY_DIR) {
                out[pos++] = '/';
            }
            out[pos++] = '\n';
        }
        (void)fs_closedir(&dir);
    }

    akira_native_critical_exit(exec_env);
    akira_free_buffer(sc);

    if (ret < 0) {
        LOG_DBG("storage_list failed: %d", ret);
        return ret;
    }

    /* Null-terminate */
    if (pos < len) {
        out[pos] = '\0';
    } else {
        out[len - 1] = '\0';
        pos = len;
    }

    return pos + 1; /* includes NUL */
}

/* --------------------------------------------------------------------------
 * App teardown
 * -------------------------------------------------------------------------- */

/* Close what the app left open. Runs after the app thread is gone (or on it,
 * after main() returned). */
static void storage_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);

    for (int i = 0; i < CONFIG_AKIRA_WASM_STORAGE_MAX_FDS; i++) {
        storage_fd_t *h = &s_fds[i];
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
            LOG_DBG("storage_release: closed fd=%d (slot %d)", i, slot);
        }
    }
}

AKIRA_APP_RESOURCE_DEFINE(storage_api, 50, NULL, storage_release);

#endif /* CONFIG_AKIRA_WASM_STORAGE */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_STORAGE))
#include <akira_native_registry.h>

static const NativeSymbol akira_storage_natives[] = {
    {"storage_open", (void *)akira_native_storage_open, "($i)i", NULL},
    {"storage_read", (void *)akira_native_storage_read, "(i*~)i", NULL},
    {"storage_write", (void *)akira_native_storage_write, "(i*~)i", NULL},
    {"storage_close", (void *)akira_native_storage_close, "(i)", NULL},
    {"storage_delete", (void *)akira_native_storage_delete, "($)i", NULL},
    {"storage_list", (void *)akira_native_storage_list, "($*~)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_storage_api, "env", akira_storage_natives);
#endif
