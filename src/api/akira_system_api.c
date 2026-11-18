/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_system
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_system, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_system_api.c
 * @brief Privileged system-level native API — SD card WASM scanner.
 *
 * sd_scan_wasm() lists all *.wasm files under /SD:/apps/ and writes their
 * filenames (newline-separated) into the caller's WASM buffer.  The function
 * bypasses the per-app storage sandbox because it requires AKIRA_CAP_APP_CONTROL,
 * an elevated capability only granted to the system shell.
 */

#include "akira_system_api.h"

#ifdef CONFIG_AKIRA_WASM_RUNTIME

#include <wasm_export.h>
#include <runtime/security.h>
#include <akira_app_containment.h>
#include "akira_common_api.h"
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

#if defined(CONFIG_AKIRA_APP_SOURCE_SD)
#include "../connectivity/storage/sd_manager.h"
#if defined(CONFIG_AKIRA_WASM_LIFECYCLE) && defined(CONFIG_AKIRA_APP_MANAGER)
#include "akira_lifecycle_api.h"
#endif
#endif

#if defined(CONFIG_AKIRA_SD_XIP)
#include "../runtime/app_manager/app_manager.h"
#endif

/* SD card WASM app directory. */
#define SD_APPS_PATH "/SD:/apps"

/* Directory entries examined per sd_scan_wasm() call: bounds how long the
 * call holds the filesystem (and how long a stop may have to wait for it). */
#define SD_SCAN_MAX_ENTRIES 256

/*
 * sd_scan_wasm(buf, buf_len) → int  (WASM signature: "(*~)i")
 *
 * Scans /SD:/apps/ for *.wasm files and writes their filenames
 * (without path, with extension) newline-separated into the caller's buffer.
 * At most SD_SCAN_MAX_ENTRIES directory entries are examined.
 *
 * Returns:
 *   ≥ 0  number of .wasm files found and written
 *  -EPERM   capability denied
 *  -EINVAL  bad pointer or buf_len == 0
 *  -ENODEV  SD card not mounted / path not found
 */
int akira_native_sd_scan_wasm(wasm_exec_env_t exec_env,
                               char *native_buf, uint32_t buf_len)
{
    /* Only the shell (holding app.control) may browse the SD directly. */
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_CONTROL, -EPERM);

    /* "(*~)i": WAMR has already validated [native_buf, native_buf + buf_len)
     * against the app's linear memory and translated it to a native pointer. */
    if (!native_buf || buf_len == 0) {
        return -EINVAL;
    }

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);

    /* The directory stays open (filesystem state) until closedir: never let a
     * stop abort the app in between. The scan below is bounded. */
    akira_native_critical_enter(exec_env);

    int ret = fs_opendir(&dir, SD_APPS_PATH);
    if (ret < 0) {
        akira_native_critical_exit(exec_env);
        /* -ENOENT when SD card is absent or directory doesn't exist */
        LOG_DBG("sd_scan_wasm: opendir '%s' failed: %d", SD_APPS_PATH, ret);
        return -ENODEV;
    }

    uint32_t offset = 0;
    int count = 0;

    for (int seen = 0; seen < SD_SCAN_MAX_ENTRIES; seen++) {
        struct fs_dirent entry;
        ret = fs_readdir(&dir, &entry);
        if (ret < 0 || entry.name[0] == '\0') {
            break; /* end of directory or error */
        }
        if (entry.type != FS_DIR_ENTRY_FILE) {
            continue; /* skip subdirectories */
        }

        /* Check for .wasm/.WASM or .aot/.AOT extension (case-insensitive).
         * FatFS may return filenames in uppercase when LFN is disabled. */
        size_t name_len = strlen(entry.name);
        bool has_wasm = name_len > 5 &&
            entry.name[name_len - 5] == '.' &&
            ((entry.name[name_len - 4] | 0x20) == 'w') &&
            ((entry.name[name_len - 3] | 0x20) == 'a') &&
            ((entry.name[name_len - 2] | 0x20) == 's') &&
            ((entry.name[name_len - 1] | 0x20) == 'm');
        bool has_aot = name_len > 4 &&
            entry.name[name_len - 4] == '.' &&
            ((entry.name[name_len - 3] | 0x20) == 'a') &&
            ((entry.name[name_len - 2] | 0x20) == 'o') &&
            ((entry.name[name_len - 1] | 0x20) == 't');
        if (!has_wasm && !has_aot) {
            continue;
        }

        /* Write "filename.wasm\n" into the buffer. */
        uint32_t needed = (uint32_t)name_len + 1U; /* +1 for '\n' */
        if (offset + needed >= buf_len) {
            LOG_DBG("sd_scan_wasm: output buffer full after %d files", count);
            break;
        }
        memcpy(native_buf + offset, entry.name, name_len);
        offset += (uint32_t)name_len;
        native_buf[offset++] = '\n';
        count++;
    }

    /* NUL-terminate the output buffer. */
    if (offset < buf_len) {
        native_buf[offset] = '\0';
    } else {
        native_buf[buf_len - 1] = '\0';
    }

    fs_closedir(&dir);
    akira_native_critical_exit(exec_env);
    LOG_DBG("sd_scan_wasm: found %d .wasm files", count);
    return count;
}

#if defined(CONFIG_AKIRA_APP_SOURCE_SD)
/* An app name, not a path: it becomes "/SD:/apps/<name>.wasm". */
static bool sd_app_name_ok(const char *name)
{
    size_t n = 0;

    if (!name || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    for (; name[n] != '\0'; n++) {
        char c = name[n];

        if (n >= 31 ||
            !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

/*
 * app_install_from_sd(name) → int  (WASM signature: "($)i")
 *
 * Installs a WASM app from the SD card into LittleFS via sd_manager_install_app().
 * The app immediately becomes available in the launcher on success.
 */
int akira_native_app_install_from_sd(wasm_exec_env_t exec_env, const char *name)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_CONTROL, -EPERM);

    if (!sd_app_name_ok(name)) {
        return -EINVAL;
    }

    /* Reinstalling a running app stops it first: never the caller itself,
     * which would have to wait for its own exit. */
    char self[32];
    if (akira_app_self_name(exec_env, self, sizeof(self)) == 0 &&
        strncmp(self, name, sizeof(self)) == 0) {
        return -EBUSY;
    }

    /* Log lines are written under the console lock in immediate mode. */
    akira_native_critical_enter(exec_env);
    LOG_INF("app_install_from_sd: installing '%s'", name);
    akira_native_critical_exit(exec_env);
#if defined(CONFIG_AKIRA_WASM_LIFECYCLE) && defined(CONFIG_AKIRA_APP_MANAGER)
    /* Seconds of SD reads and flash writes under the registry and filesystem
     * locks: run on the lifecycle queue, never on the app's thread, so an
     * abort can't leak them. This waits for the result (stop-aware). */
    int ret = akira_lifecycle_run_deferred(exec_env, sd_manager_install_app, name);
#else
    /* No lifecycle queue in this build: the stop path waits for it instead
     * of aborting in the middle. */
    akira_native_critical_enter(exec_env);
    int ret = sd_manager_install_app(name);
    akira_native_critical_exit(exec_env);
#endif
    akira_native_critical_enter(exec_env);
    if (ret < 0) {
        LOG_WRN("app_install_from_sd: failed (%d) for '%s'", ret, name);
    } else {
        LOG_INF("app_install_from_sd: '%s' installed (id=%d)", name, ret);
    }
    akira_native_critical_exit(exec_env);
    return ret;
}
#endif /* CONFIG_AKIRA_APP_SOURCE_SD */

#if defined(CONFIG_AKIRA_SD_XIP)
/*
 * app_run_from_sd(name) → int  (WASM signature: "($)i")
 *
 * Loads and runs a WASM app directly from SD card without installing it.
 * The app is executed transiently in PSRAM — nothing is written to flash.
 */
int akira_native_app_run_from_sd(wasm_exec_env_t exec_env, const char *name)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_CONTROL, -EACCES);

    if (!name || name[0] == '\0') {
        return -EINVAL;
    }

    LOG_INF("app_run_from_sd: running '%s' from SD", name);
    int ret = app_manager_run_from_sd(name);
    if (ret < 0) {
        LOG_WRN("app_run_from_sd: failed (%d) for '%s'", ret, name);
    }
    return ret;
}
#endif /* CONFIG_AKIRA_SD_XIP */

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_SYSTEM_API))
#include <akira_native_registry.h>

/* system: privileged SD card scan (requires app.control cap) */
static const NativeSymbol akira_system_natives[] = {
    {"sd_scan_wasm", (void *)akira_native_sd_scan_wasm, "(*~)i", NULL},
#if defined(CONFIG_AKIRA_APP_SOURCE_SD)
    {"app_install_from_sd", (void *)akira_native_app_install_from_sd, "($)i", NULL},
#endif
};

AKIRA_NATIVE_API_DEFINE(akira_system_api, "env", akira_system_natives);
#endif
