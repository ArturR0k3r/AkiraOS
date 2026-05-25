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
 * caller's WASM buffer.  Returns the number of files found (≥ 0) or a
 * negative Zephyr errno on error (-ENODEV if no SD card, -EACCES if
 * capability check fails, -ENOMEM if the buffer is too small).
 *
 * Capability: "app.control" (AKIRA_CAP_APP_CONTROL)
 */
int akira_native_sd_scan_wasm(wasm_exec_env_t exec_env,
                               uint32_t buf_ptr, uint32_t buf_len);

#if defined(CONFIG_AKIRA_APP_SOURCE_SD)
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
