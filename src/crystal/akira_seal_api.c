/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_seal_api.c
 * @brief WASM natives for the Seal and Akira, and the native-API gate.
 *
 *   int seal_request(int op, const char *reason)    1 granted, 0 refused
 *   int akira_glint(int achievement_id)             1 new glint, 0 not
 *   int seal_record(const char *what, int64 value)  1 sealed, 0 refused
 *
 * None needs a capability: asking is always allowed, granting is the owner's.
 */

#include "akira_seal.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <runtime/security.h>
#include <akira_app_containment.h>

LOG_MODULE_REGISTER(akira_seal_api, CONFIG_AKIRA_LOG_LEVEL);

#if defined(CONFIG_AKIRA_WASM_RUNTIME)

/* No name, no identity: never fall back to a name apps could share. */
static bool app_name(wasm_exec_env_t env, char *buf, size_t len)
{
    wasm_module_inst_t inst = env ? wasm_runtime_get_module_inst(env) : NULL;

    return inst && akira_runtime_get_name_for_module_inst(inst, buf, len) == 0 && buf[0];
}

/*
 * Each "always"-granted record is an SE050 signature plus a ledger append in
 * flash: a burst of 4, then one every 5 s, for all apps together.
 */
static bool record_allowed(void)
{
    static struct k_spinlock lock;
    static int64_t stamp;
    static int tokens = 4;
    k_spinlock_key_t key = k_spin_lock(&lock);
    int64_t now = k_uptime_get();
    bool ok;

    while (tokens < 4 && now - stamp >= 5000) {
        tokens++;
        stamp += 5000;
    }
    if (tokens >= 4) {
        stamp = now;
    }
    ok = tokens > 0;
    if (ok) {
        tokens--;
    }
    k_spin_unlock(&lock, key);
    return ok;
}

int akira_seal_gate(wasm_exec_env_t exec_env, akira_seal_op_t op, const char *detail)
{
    char app[AKIRA_APP_NAME_LEN];
    bool granted;

    if (!app_name(exec_env, app, sizeof(app))) {
        return -EACCES;
    }
    /* The hot path (one lookup per packet / keystroke) takes the Seal's lock:
     * a stopping app is never cut off while holding it. */
    akira_native_critical_enter(exec_env);
    granted = akira_seal_granted(app, op);
    akira_native_critical_exit(exec_env);
    if (granted) {
        return 0;
    }
    /* Waiting for the owner is safe to stop: stopping the app answers "no". */
    return akira_seal_require(app, op, detail);
}

int akira_native_seal_request(wasm_exec_env_t exec_env, int32_t op, const char *reason)
{
    if (op <= AKIRA_SEAL_NONE || op >= AKIRA_SEAL_OP_COUNT ||
        akira_seal_op_is_system((akira_seal_op_t)op) || op == AKIRA_SEAL_RECORD) {
        return -EINVAL;
    }
    return akira_seal_gate(exec_env, (akira_seal_op_t)op, reason) == 0 ? 1 : 0;
}

int akira_native_akira_glint(wasm_exec_env_t exec_env, int32_t id)
{
    char app[AKIRA_APP_NAME_LEN];
    bool ok;

    if (!app_name(exec_env, app, sizeof(app))) {
        return 0;
    }
    akira_native_critical_enter(exec_env);
    ok = akira_crystal_add_glint(app, (uint32_t)id);
    akira_native_critical_exit(exec_env);
    return ok ? 1 : 0;
}

int akira_native_seal_record(wasm_exec_env_t exec_env, const char *what, int64_t value)
{
    char app[AKIRA_APP_NAME_LEN];
    int ret;

    if (!app_name(exec_env, app, sizeof(app)) || !record_allowed()) {
        return 0;
    }
    /* Already "always": signs and appends to the ledger under its locks, so
     * a stop waits for it. Otherwise the owner is asked, and stopping the app
     * ends that wait at once (it counts as "no"). */
    akira_native_critical_enter(exec_env);
    bool always = akira_seal_granted(app, AKIRA_SEAL_RECORD);

    if (!always) {
        akira_native_critical_exit(exec_env);
    }
    ret = akira_seal_record(app, what, value);
    if (always) {
        akira_native_critical_exit(exec_env);
    }
    return ret == 0 ? 1 : 0;
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
