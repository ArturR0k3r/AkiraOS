/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_settings_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_settings_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_settings_api.c
 * @brief WASM native bridge for persistent key-value settings (NVS).
 */

#include "akira_settings_api.h"
#include <runtime/security.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_SETTINGS

#include <wasm_export.h>
#include "settings/settings.h"

/* Pointer-validation helper shared across API modules */
static inline void *wasm_ptr_to_native(wasm_exec_env_t exec_env,
                                        uint32_t wasm_ptr, uint32_t len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst) return NULL;
    if (!wasm_runtime_validate_app_addr(inst, wasm_ptr, len)) return NULL;
    return wasm_runtime_addr_app_to_native(inst, wasm_ptr);
}

/* WASM capability for settings — reuse AKIRA_CAP_SETTINGS defined in security.h */
#ifndef AKIRA_CAP_SETTINGS
#define AKIRA_CAP_SETTINGS (1U << 23)
#endif

/* ── settings_get ─────────────────────────────────────────────────────── */
int akira_native_settings_get(wasm_exec_env_t exec_env,
                               const char *key,
                               uint32_t buf_ptr, uint32_t buf_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_SETTINGS, -EACCES);

    if (!key || key[0] == '\0' || buf_len == 0) return -EINVAL;

    char *buf = (char *)wasm_ptr_to_native(exec_env, buf_ptr, buf_len);
    if (!buf) return -EFAULT;

    /* Use native settings layer */
    int ret = akira_settings_get(key, buf, (size_t)buf_len);
    if (ret < 0) {
        /* Ensure buffer is NUL-terminated even on error */
        buf[0] = '\0';
    }
    return ret;
}

/* ── settings_set ─────────────────────────────────────────────────────── */
int akira_native_settings_set(wasm_exec_env_t exec_env,
                               const char *key, const char *value)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_SETTINGS, -EACCES);

    if (!key || key[0] == '\0') return -EINVAL;
    if (!value) return -EINVAL;

    return akira_settings_set(key, value, 0 /* not encrypted */);
}

/* ── settings_delete ──────────────────────────────────────────────────── */
int akira_native_settings_delete(wasm_exec_env_t exec_env, const char *key)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_SETTINGS, -EACCES);

    if (!key || key[0] == '\0') return -EINVAL;

    return akira_settings_delete(key);
}

#endif /* CONFIG_AKIRA_WASM_SETTINGS */
