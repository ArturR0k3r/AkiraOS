/*
 * Copyright (c) 2026 PenEngineering S.R.L
* SPDX-License-Identifier: Apache-2.0
 */

#ifndef AKIRA_SETTINGS_API_H
#define AKIRA_SETTINGS_API_H

/**
 * @file akira_settings_api.h
 * @brief WASM native bridge for persistent key-value settings (NVS).
 *
 * Allows WASM apps to read and write persistent settings backed by
 * Zephyr NVS flash.  Requires the "settings.*" capability.
 *
 * Key format:  "namespace/key"   e.g. "nes/frameskip", "wifi/ssid"
 * Values are plain strings (max AKIRA_SETTINGS_WASM_VAL_MAX bytes).
 */

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

#ifdef CONFIG_AKIRA_WASM_SETTINGS

/**
 * @brief Read a persistent setting.
 *
 * @param exec_env  WAMR execution environment (for capability check).
 * @param key       NUL-terminated setting key (e.g. "nes/frameskip").
 * @param buf_ptr   WASM linear-memory pointer to output buffer.
 * @param buf_len   Size of output buffer in bytes.
 * @return 0 on success, -ENOENT if key not found, -EACCES if no capability,
 *         -EINVAL on bad args, -EFAULT on invalid WASM pointer.
 */
int akira_native_settings_get(wasm_exec_env_t exec_env,
                               const char *key,
                               uint32_t buf_ptr, uint32_t buf_len);

/**
 * @brief Write a persistent setting.
 *
 * @param exec_env  WAMR execution environment (for capability check).
 * @param key       NUL-terminated setting key.
 * @param value     NUL-terminated value string.
 * @return 0 on success, -EACCES if no capability, -EINVAL on bad args,
 *         negative errno on storage failure.
 */
int akira_native_settings_set(wasm_exec_env_t exec_env,
                               const char *key, const char *value);

/**
 * @brief Delete a persistent setting.
 *
 * @param exec_env  WAMR execution environment (for capability check).
 * @param key       NUL-terminated setting key.
 * @return 0 on success, -ENOENT if key not found, -EACCES if no capability.
 */
int akira_native_settings_delete(wasm_exec_env_t exec_env, const char *key);

#else  /* !CONFIG_AKIRA_WASM_SETTINGS — stub declarations */

static inline int akira_native_settings_get(void *e, const char *k,
                                             uint32_t p, uint32_t l)
{ (void)e;(void)k;(void)p;(void)l; return -ENOTSUP; }

static inline int akira_native_settings_set(void *e, const char *k,
                                             const char *v)
{ (void)e;(void)k;(void)v; return -ENOTSUP; }

static inline int akira_native_settings_delete(void *e, const char *k)
{ (void)e;(void)k; return -ENOTSUP; }

#endif /* CONFIG_AKIRA_WASM_SETTINGS */

#endif /* AKIRA_SETTINGS_API_H */
