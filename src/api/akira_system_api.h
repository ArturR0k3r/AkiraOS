/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file akira_system_api.h
 * @brief Privileged system-level native APIs.
 *
 * Currently exposes one function:
 *   sd_scan_wasm(buf, len) — list *.wasm files under /SD:/apps/
 *
 * Requires elevated capability: AKIRA_CAP_APP_CONTROL ("app.control").
 * Only the system shell (akira_shell) should hold this capability.
 */

/**
 * @file akira_system_api.h
 * @stability stable
 * @since 1.4
 */
#ifndef AKIRA_SYSTEM_API_H
#define AKIRA_SYSTEM_API_H

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>

/**
 * sd_scan_wasm(buf_ptr, buf_len) → int  (WASM signature: "(*~)i")
 *
 * Scans /SD:/apps/ and writes newline-separated *.wasm filenames into the
 * caller's WASM buffer (WAMR validates [buf, buf + buf_len)).  At most 256
 * directory entries are examined per call; names that don't fit are left
 * out.  Returns the number of files written (≥ 0) or a negative Zephyr
 * errno on error (-ENODEV if no SD card, -EPERM if the capability check
 * fails, -EINVAL on a bad buffer).
 *
 * Capability: "app.control" (AKIRA_CAP_APP_CONTROL)
 */
int akira_native_sd_scan_wasm(wasm_exec_env_t exec_env,
                               char *native_buf, uint32_t buf_len);

#if defined(CONFIG_AKIRA_APP_SOURCE_SD)
/**
 * app_install_from_sd(name) → int  (WASM signature: "($)i")
 *
 * Copies /SD:/apps/<name>.wasm into the LittleFS app store and registers it
 * with the app manager, so it appears immediately in the launcher without
 * any reboot or PC connection.
 *
 * The copy runs on the lifecycle work queue when the build has one (the
 * caller waits for it, stop-aware; see akira_lifecycle_run_deferred()).
 *
 * @param name  App name string (without .wasm extension, null-terminated;
 *              letters, digits, '_', '-', '.', at most 31 characters)
 * @return app id (≥ 0) on success, negative Zephyr errno on error:
 *   -EPERM   capability "app.control" not granted
 *   -EINVAL  NULL, empty or invalid name
 *   -EBUSY   @p name is the calling app itself
 *   -EAGAIN / -EBUSY / -EINTR  see akira_lifecycle_run_deferred()
 *   -ENOENT  file not found on SD card
 *   -ENOSPC  LittleFS full
 *   -ENODEV  SD card not mounted
 *
 * Capability: "app.control" (AKIRA_CAP_APP_CONTROL)
 */
int akira_native_app_install_from_sd(wasm_exec_env_t exec_env, const char *name);
#endif /* CONFIG_AKIRA_APP_SOURCE_SD */

#if defined(CONFIG_AKIRA_SD_XIP)
/**
 * app_run_from_sd(name) → int  (WASM signature: "($)i")
 *
 * Loads and runs a WASM app directly from SD card without installing it.
 * The app runs transiently from PSRAM; nothing is written to flash.
 */
int akira_native_app_run_from_sd(wasm_exec_env_t exec_env, const char *name);
#endif /* CONFIG_AKIRA_SD_XIP */

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#endif /* AKIRA_SYSTEM_API_H */
