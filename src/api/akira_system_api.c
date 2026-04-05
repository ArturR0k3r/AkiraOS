/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
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
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

#if defined(CONFIG_AKIRA_APP_SOURCE_SD)
#include "../connectivity/storage/sd_manager.h"
#endif

/* SD card WASM app directory. */
#define SD_APPS_PATH "/SD:/apps"

/*
 * sd_scan_wasm(buf_ptr, buf_len) → int  (WASM signature: "(*~)i")
 *
 * Scans /SD:/apps/ for *.wasm files and writes their filenames
 * (without path, with extension) newline-separated into the caller's buffer.
 *
 * Returns:
 *   ≥ 0  number of .wasm files found and written
 *  -EACCES  capability denied
 *  -EINVAL  bad pointer or buf_len == 0
 *  -ENODEV  SD card not mounted / path not found
 *  -ENOMEM  output buffer too small to hold all names
 */
int akira_native_sd_scan_wasm(wasm_exec_env_t exec_env,
                               uint32_t buf_ptr, uint32_t buf_len)
{
    /* Only the shell (holding app.control) may browse the SD directly. */
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_CONTROL, -EACCES);

    if (buf_len == 0) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst) {
        return -EINVAL;
    }

    char *native_buf = (char *)wasm_runtime_addr_app_to_native(inst, buf_ptr);
    if (!native_buf) {
        LOG_ERR("sd_scan_wasm: invalid WASM pointer 0x%08x", buf_ptr);
        return -EINVAL;
    }

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);

    int ret = fs_opendir(&dir, SD_APPS_PATH);
    if (ret < 0) {
        /* -ENOENT when SD card is absent or directory doesn't exist */
        LOG_INF("sd_scan_wasm: opendir '%s' failed: %d", SD_APPS_PATH, ret);
        return -ENODEV;
    }

    uint32_t offset = 0;
    int count = 0;

    while (true) {
        struct fs_dirent entry;
        ret = fs_readdir(&dir, &entry);
        if (ret < 0 || entry.name[0] == '\0') {
            break; /* end of directory or error */
        }
        if (entry.type != FS_DIR_ENTRY_FILE) {
            continue; /* skip subdirectories */
        }

        /* Check for .wasm extension (case-sensitive). */
        size_t name_len = strlen(entry.name);
        if (name_len < 5 ||
            entry.name[name_len - 5] != '.' ||
            entry.name[name_len - 4] != 'w' ||
            entry.name[name_len - 3] != 'a' ||
            entry.name[name_len - 2] != 's' ||
            entry.name[name_len - 1] != 'm') {
            continue;
        }

        /* Write "filename.wasm\n" into the buffer. */
        uint32_t needed = (uint32_t)name_len + 1U; /* +1 for '\n' */
        if (offset + needed >= buf_len) {
            LOG_WRN("sd_scan_wasm: output buffer full after %d files", count);
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
    LOG_DBG("sd_scan_wasm: found %d .wasm files", count);
    return count;
}

#if defined(CONFIG_AKIRA_APP_SOURCE_SD)
/*
 * app_install_from_sd(name) → int  (WASM signature: "($)i")
 *
 * Installs a WASM app from the SD card into LittleFS via sd_manager_install_app().
 * The app immediately becomes available in the launcher on success.
 */
int akira_native_app_install_from_sd(wasm_exec_env_t exec_env, const char *name)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_CONTROL, -EACCES);

    if (!name || name[0] == '\0') {
        return -EINVAL;
    }

    LOG_INF("app_install_from_sd: installing '%s'", name);
    int ret = sd_manager_install_app(name);
    if (ret < 0) {
        LOG_WRN("app_install_from_sd: failed (%d) for '%s'", ret, name);
    } else {
        LOG_INF("app_install_from_sd: '%s' installed (id=%d)", name, ret);
    }
    return ret;
}
#endif /* CONFIG_AKIRA_APP_SOURCE_SD */

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
