/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_ota_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_ota_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_ota_api.c
 * @brief OTA update WASM native API implementation.
 *
 * Security:
 *   - Only HTTPS URLs are accepted (http:// rejected with -EPROTO).
 *   - The manifest URL is validated before any network operation.
 *   - WASM sandbox cannot write flash directly — only via this native API.
 *   - Requires AKIRA_CAP_OTA_TRIGGER on the calling app.
 *
 * Gate: CONFIG_AKIRA_WASM_OTA=y
 */

#ifdef CONFIG_AKIRA_WASM_OTA

#include "akira_ota_api.h"
#include <runtime/security.h>
#include <runtime/akira_runtime.h>
#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <connectivity/ota/ota_manager.h>
#endif
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

#ifdef CONFIG_AKIRA_BOOT_GUARD
#include <akira_boot_guard.h>
#endif

#ifdef CONFIG_AKIRA_TELEMETRY
#include <lib/akira_telemetry.h>
#endif

#define MANIFEST_URL_MAX 256

/* ── URL security check ─────────────────────────────────────────────────── */

static int validate_https_url(const char *url)
{
    if (!url || url[0] == '\0') {
        return -EINVAL;
    }
    if (strncmp(url, "https://", 8) != 0) {
        LOG_WRN("OTA API: only HTTPS URLs allowed (got '%.*s...')", 16, url);
        return -EPROTO;
    }
    return 0;
}

/* ── ota_check ──────────────────────────────────────────────────────────── */

int akira_native_ota_check(wasm_exec_env_t exec_env, const char *manifest_url)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_OTA_TRIGGER, -EACCES);

    if (!manifest_url) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_native_addr(inst, (void *)manifest_url,
                                           strnlen(manifest_url,
                                                   MANIFEST_URL_MAX) + 1)) {
        return -EFAULT;
    }

    int ret = validate_https_url(manifest_url);
    if (ret < 0) {
        return ret;
    }

    /* TODO: Fetch manifest JSON over HTTP(S) and compare version field
     *       against AKIRA_VERSION_MAJOR.MINOR.PATCH.
     *       For now return 0 (no update) to allow native_sim testing. */
    LOG_INF("OTA check: manifest URL validated (network fetch: stub)");
    return 0;
}

/* ── ota_fetch_and_apply ────────────────────────────────────────────────── */

int akira_native_ota_fetch_and_apply(wasm_exec_env_t exec_env,
                                      const char *manifest_url)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_OTA_TRIGGER, -EACCES);

    if (!manifest_url) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_native_addr(inst, (void *)manifest_url,
                                           strnlen(manifest_url,
                                                   MANIFEST_URL_MAX) + 1)) {
        return -EFAULT;
    }

    int ret = validate_https_url(manifest_url);
    if (ret < 0) {
        return ret;
    }

    /* Check OTA is not already in progress */
#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
    const struct ota_progress *p = ota_get_progress();
    if (p && p->state == OTA_STATE_IN_PROGRESS) {
        return -EBUSY;
    }
#endif

#ifdef CONFIG_AKIRA_TELEMETRY
    akira_telemetry_ota_progress(0, 0);
#endif

    /* TODO: Full manifest parse + HTTP download + write_chunk loop.
     *       Stub: log only. */
    LOG_INF("OTA fetch_and_apply: stub (real download deferred to connectivity integration)");
    return 0;
}

/* ── ota_get_state ──────────────────────────────────────────────────────── */

int akira_native_ota_get_state(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_OTA_TRIGGER, -EACCES);

#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
    const struct ota_progress *p = ota_get_progress();
    if (!p) {
        return -ENODEV;
    }
    return (int)p->state;
#else
    return -ENOTSUP;
#endif
}

/* ── ota_confirm ────────────────────────────────────────────────────────── */

int akira_native_ota_confirm(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_OTA_TRIGGER, -EACCES);

#ifdef CONFIG_AKIRA_BOOT_GUARD
    return akira_boot_guard_confirm();
#elif defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
    enum ota_result r = ota_confirm_firmware();
    return (r == OTA_OK) ? 0 : -EIO;
#else
    return -ENOTSUP;
#endif
}

/* ── ota_rollback ───────────────────────────────────────────────────────── */

int akira_native_ota_rollback(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_OTA_TRIGGER, -EACCES);

#if defined(CONFIG_FLASH_MAP) && defined(CONFIG_BOOTLOADER_MCUBOOT)
    enum ota_result r = ota_request_rollback();
    return (r == OTA_OK) ? 0 : -EIO;
#else
    return -ENOTSUP;
#endif
}

#endif /* CONFIG_AKIRA_WASM_OTA */
