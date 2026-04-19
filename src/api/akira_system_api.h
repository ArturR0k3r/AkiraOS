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

#ifndef AKIRA_SYSTEM_API_H
#define AKIRA_SYSTEM_API_H

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>

/**
 * sd_scan_wasm(buf_ptr, buf_len) → int  (WASM signature: "(*~)i")
 *
 * Scans /SD:/apps/ and writes newline-separated *.wasm filenames into the
 * caller's WASM buffer.  Returns the number of files found (≥ 0) or a
 * negative Zephyr errno on error (-ENODEV if no SD card, -EACCES if
 * capability check fails, -ENOMEM if the buffer is too small).
 *
 * Capability: "app.control" (AKIRA_CAP_APP_CONTROL)
 */
int akira_native_sd_scan_wasm(wasm_exec_env_t exec_env,
                               uint32_t buf_ptr, uint32_t buf_len);

#if defined(CONFIG_AKIRA_APP_SOURCE_SD)
/**
 * app_install_from_sd(name) → int  (WASM signature: "($)i")
 *
 * Copies /SD:/apps/<name>.wasm into the LittleFS app store and registers it
 * with the app manager, so it appears immediately in the launcher without
 * any reboot or PC connection.
 *
 * @param name  App name string (without .wasm extension, null-terminated)
 * @return 0 on success, negative Zephyr errno on error:
 *   -EACCES  capability "app.control" not granted
 *   -EINVAL  NULL or empty name
 *   -ENOENT  file not found on SD card
 *   -ENOSPC  LittleFS full
 *   -ENODEV  SD card not mounted
 *
 * Capability: "app.control" (AKIRA_CAP_APP_CONTROL)
 */
int akira_native_app_install_from_sd(wasm_exec_env_t exec_env, const char *name);
#endif /* CONFIG_AKIRA_APP_SOURCE_SD */

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#endif /* AKIRA_SYSTEM_API_H */
