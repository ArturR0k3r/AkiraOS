/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_ipc_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_ipc_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_ipc_api.c
 * @brief IPC WASM native API wrappers
 *
 * The calling app's identity is resolved from exec_env on every call so
 * WASM apps cannot impersonate each other or receive each other's messages.
 */

#include "akira_ipc_api.h"
#include "akira_common_api.h"
#include "akira_wasm_mem.h"
#include <runtime/security.h>
#include <runtime/akira_ipc.h>
#include <akira_app_containment.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

/* A long or unbounded msg_recv waits in slices of this length, checking
 * between them whether the app is being stopped (a stop also wakes the
 * slice itself, see ipc_on_stop in akira_ipc.c). */
#define IPC_RECV_SLICE_MS 100

/* ── Internal helper ─────────────────────────────────────────────────────── */

/* Subscriptions belong to the caller's runtime slot, so an app can neither
 * read nor remove another app's queue, and whatever it holds is released
 * when it ends. No fallback identity: a subscription not tied to a slot
 * would outlive the app. */
static int caller_slot(wasm_exec_env_t exec_env)
{
    int slot = akira_runtime_slot_of_exec_env(exec_env);

    return slot >= 0 ? slot : -EINVAL;
}

/* ── WASM Exports ──────────────────────────────────────────────────────────
 * Every akira_ipc_* call takes the IPC mutex: each is bracketed as a native
 * critical section so a stop never aborts the app while it holds it. */

int akira_native_msg_subscribe(wasm_exec_env_t exec_env, const char *topic)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_IPC, -EPERM);

    if (!topic || topic[0] == '\0') {
        return -EINVAL;
    }

    int slot = caller_slot(exec_env);
    if (slot < 0) {
        return slot;
    }

    char caller[AKIRA_IPC_APP_NAME_MAX];
    (void)akira_app_self_name(exec_env, caller, sizeof(caller));

    akira_native_critical_enter(exec_env);
    int ret = akira_ipc_subscribe_slot(slot, topic, caller);
    akira_native_critical_exit(exec_env);
    return ret;
}

int akira_native_msg_unsubscribe(wasm_exec_env_t exec_env, const char *topic)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_IPC, -EPERM);

    if (!topic || topic[0] == '\0') {
        return -EINVAL;
    }

    int slot = caller_slot(exec_env);
    if (slot < 0) {
        return slot;
    }

    akira_native_critical_enter(exec_env);
    int ret = akira_ipc_unsubscribe_slot(slot, topic);
    akira_native_critical_exit(exec_env);
    return ret;
}

int akira_native_msg_publish(wasm_exec_env_t exec_env,
                             const char *topic,
                             uint32_t data_ptr, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_IPC, -EPERM);

    if (!topic || topic[0] == '\0') {
        return -EINVAL;
    }

    if (len == 0 || len > CONFIG_AKIRA_IPC_MSG_MAX_SIZE) {
        return -EMSGSIZE;
    }

    /* Validates the whole [data_ptr, data_ptr + len) range. */
    const void *ptr = akira_wasm_ptr_to_native(exec_env, data_ptr, len);
    if (!ptr) {
        return -EFAULT;
    }

    akira_native_critical_enter(exec_env);
    int ret = akira_ipc_publish(topic, ptr, len);
    akira_native_critical_exit(exec_env);
    return ret;
}

int akira_native_msg_recv(wasm_exec_env_t exec_env,
                          const char *topic,
                          uint32_t buf_ptr, uint32_t buf_len,
                          int32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_IPC, -EPERM);

    if (!topic || topic[0] == '\0' || buf_len == 0) {
        return -EINVAL;
    }

    /* Validates the whole [buf_ptr, buf_ptr + buf_len) range. */
    void *ptr = akira_wasm_ptr_to_native(exec_env, buf_ptr, buf_len);
    if (!ptr) {
        return -EFAULT;
    }

    int slot = caller_slot(exec_env);
    if (slot < 0) {
        return slot;
    }

    /* Never one long (or K_FOREVER) wait: slices of IPC_RECV_SLICE_MS with
     * a stop check in between, each re-resolving the subscription. A slice
     * holds the IPC mutex only briefly but is bracketed as a whole (it is
     * bounded), so the abort path never lands inside the locked part. */
    int64_t deadline = (timeout_ms > 0) ? k_uptime_get() + timeout_ms : 0;
    int ret;

    do {
        if (akira_runtime_stop_requested(exec_env)) {
            return -EINTR;
        }

        k_timeout_t slice = K_NO_WAIT;
        if (timeout_ms < 0) {
            slice = K_MSEC(IPC_RECV_SLICE_MS);
        } else if (timeout_ms > 0) {
            int64_t left = deadline - k_uptime_get();
            if (left <= 0) {
                return -EAGAIN;
            }
            slice = K_MSEC(MIN(left, (int64_t)IPC_RECV_SLICE_MS));
        }

        akira_native_critical_enter(exec_env);
        ret = akira_ipc_recv_slot(slot, topic, ptr, buf_len, slice);
        akira_native_critical_exit(exec_env);
    } while (ret == -EAGAIN && timeout_ms != 0);

    return ret;
}

int akira_native_msg_try_recv(wasm_exec_env_t exec_env,
                              const char *topic,
                              uint32_t buf_ptr, uint32_t buf_len)
{
    return akira_native_msg_recv(exec_env, topic, buf_ptr, buf_len, 0);
}

int akira_native_msg_pending(wasm_exec_env_t exec_env, const char *topic)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_IPC, -EPERM);

    if (!topic || topic[0] == '\0') {
        return -EINVAL;
    }

    int slot = caller_slot(exec_env);
    if (slot < 0) {
        return slot;
    }

    akira_native_critical_enter(exec_env);
    int ret = akira_ipc_pending_slot(slot, topic);
    akira_native_critical_exit(exec_env);
    return ret;
}

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_IPC))
#include <akira_native_registry.h>

static const NativeSymbol akira_ipc_natives[] = {
    {"msg_subscribe", (void *)akira_native_msg_subscribe, "($)i", NULL},
    {"msg_unsubscribe", (void *)akira_native_msg_unsubscribe, "($)i", NULL},
    {"msg_publish", (void *)akira_native_msg_publish, "($ii)i", NULL},
    {"msg_recv", (void *)akira_native_msg_recv, "($iii)i", NULL},
    {"msg_try_recv", (void *)akira_native_msg_try_recv, "($ii)i", NULL},
    {"msg_pending", (void *)akira_native_msg_pending, "($)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_ipc_api, "env", akira_ipc_natives);
#endif
