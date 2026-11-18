/*
 * Copyright (c) 2026 PenEngineering S.R.L
* SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_settings_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_settings_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_settings_api.c
 * @brief WASM native bridge for persistent key-value settings (NVS).
 *
 * Every key an app uses lives in a namespace of its own
 * (akira_settings_app_call()), so the OS's keys (Wi-Fi credentials, MQTT
 * password, mesh identity, boot guard record...) and other apps' keys are out
 * of reach whatever name the app passes. Requests are bounded waits that end
 * when the app is stopped, keys per app are capped, and writes are
 * rate-limited per app so no app can fill or wear out the settings partition.
 */

#include "akira_settings_api.h"
#include <runtime/security.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

#ifdef CONFIG_AKIRA_WASM_SETTINGS

#include <wasm_export.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <akira_app_containment.h>
#include <runtime/akira_runtime.h>
#include "akira_wasm_mem.h"
#include "settings/settings.h"

/* WASM capability for settings — reuse AKIRA_CAP_SETTINGS defined in security.h */
#ifndef AKIRA_CAP_SETTINGS
#define AKIRA_CAP_SETTINGS (1U << 23)
#endif

/* Longest the app thread waits for the settings queue. Shorter than the
 * runtime's stop timeout, and a stop ends the wait at once anyway. */
#define APP_SETTINGS_WAIT K_SECONDS(4)

/* Writes (set/delete) per app: a burst big enough to save a whole app state
 * at once, then one every WRITE_REFILL_MS. Flash wear stays bounded however
 * long an app keeps writing (NVS skips writes of unchanged values). */
#define WRITE_BURST     40
#define WRITE_REFILL_MS 2000

static struct {
    int64_t  stamp;
    uint16_t tokens;
    bool     primed;
} s_writes[AKIRA_MAX_WASM_INSTANCES];
static struct k_spinlock s_writes_lock;

/* Per slot, not reset when the app ends: restarting doesn't refill it. */
static bool write_allowed(int slot)
{
    bool ok;
    k_spinlock_key_t key = k_spin_lock(&s_writes_lock);
    int64_t now = k_uptime_get();

    if (!s_writes[slot].primed) {
        s_writes[slot].primed = true;
        s_writes[slot].tokens = WRITE_BURST;
        s_writes[slot].stamp  = now;
    }
    while (s_writes[slot].tokens < WRITE_BURST &&
           now - s_writes[slot].stamp >= WRITE_REFILL_MS) {
        s_writes[slot].tokens++;
        s_writes[slot].stamp += WRITE_REFILL_MS;
    }
    if (s_writes[slot].tokens >= WRITE_BURST) {
        s_writes[slot].stamp = now;
    }
    ok = s_writes[slot].tokens > 0;
    if (ok) {
        s_writes[slot].tokens--;
    }
    k_spin_unlock(&s_writes_lock, key);
    return ok;
}

static bool app_stopping(void *ctx)
{
    return akira_runtime_stop_requested((wasm_exec_env_t)ctx);
}

/* One request in the calling app's namespace. @p key and @p value point into
 * app memory: akira_settings_app_call() copies them before it waits. */
static int app_settings(wasm_exec_env_t exec_env, settings_op_type_t op,
                        const char *key, const char *value, char *out, size_t out_len)
{
    char app[32];
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    int slot = akira_runtime_slot_of_exec_env(exec_env);

    if (!key || key[0] == '\0') {
        return -EINVAL;
    }
    if (!inst || slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES ||
        akira_runtime_get_name_for_module_inst(inst, app, sizeof(app)) != 0 ||
        app[0] == '\0') {
        return -EPERM;
    }
    if (op != AKIRA_SETTINGS_OP_GET && !write_allowed(slot)) {
        LOG_DBG("settings: write rate limit for '%s'", app);
        return -EAGAIN;
    }
    return akira_settings_app_call(op, app, key, value, out, out_len,
                                   APP_SETTINGS_WAIT, app_stopping, exec_env);
}

/* ── settings_get ("($ii)i": key string, raw buffer offset, length) ────── */
int akira_native_settings_get(wasm_exec_env_t exec_env,
                               const char *key,
                               uint32_t buf_ptr, uint32_t buf_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_SETTINGS, -EPERM);

    if (!key || key[0] == '\0' || buf_len == 0) return -EINVAL;

    /* The whole buffer must be the app's before anything else happens. */
    if (!akira_wasm_ptr_to_native(exec_env, buf_ptr, buf_len)) return -EFAULT;

    /* The value comes back in a host buffer and is copied in afterwards,
     * re-checking the app's buffer: the settings thread never writes app
     * memory. */
    char val[MAX_VALUE_LEN];
    size_t want = MIN((size_t)buf_len, sizeof(val));
    int ret = app_settings(exec_env, AKIRA_SETTINGS_OP_GET, key, NULL, val, want);

    char *buf = (char *)akira_wasm_ptr_to_native(exec_env, buf_ptr, buf_len);
    if (!buf) return -EFAULT;

    if (ret < 0) {
        /* Ensure buffer is NUL-terminated even on error */
        buf[0] = '\0';
        return ret;
    }
    size_t n = 0;
    while (n + 1 < want && val[n] != '\0') {
        n++;
    }
    memcpy(buf, val, n);
    buf[n] = '\0';
    return ret;
}

/* ── settings_set ─────────────────────────────────────────────────────── */
int akira_native_settings_set(wasm_exec_env_t exec_env,
                               const char *key, const char *value)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_SETTINGS, -EPERM);

    if (!key || key[0] == '\0') return -EINVAL;
    if (!value) return -EINVAL;

    return app_settings(exec_env, AKIRA_SETTINGS_OP_SET, key, value, NULL, 0);
}

/* ── settings_delete ──────────────────────────────────────────────────── */
int akira_native_settings_delete(wasm_exec_env_t exec_env, const char *key)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_SETTINGS, -EPERM);

    if (!key || key[0] == '\0') return -EINVAL;

    return app_settings(exec_env, AKIRA_SETTINGS_OP_DELETE, key, NULL, NULL, 0);
}

#endif /* CONFIG_AKIRA_WASM_SETTINGS */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_SETTINGS))
#include <akira_native_registry.h>

/* settings.*: persistent NVS key-value store */
static const NativeSymbol akira_settings_natives[] = {
    {"settings_get", (void *)akira_native_settings_get, "($ii)i", NULL},
    {"settings_set", (void *)akira_native_settings_set, "($$)i", NULL},
    {"settings_delete", (void *)akira_native_settings_delete, "($)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_settings_api, "env", akira_settings_natives);
#endif
