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
#include <runtime/security.h>
#include <runtime/akira_ipc.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

/* ── Internal helper ─────────────────────────────────────────────────────── */

/**
 * @brief Resolve the calling app's name from exec_env into @p buf.
 * @return 0 on success, negative on error
 */
static int resolve_caller(wasm_exec_env_t exec_env,
                           char *buf, size_t buflen)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst) {
        buf[0] = '\0';
        return -EINVAL;
    }
    int ret = akira_runtime_get_name_for_module_inst(inst, buf, buflen);
    if (ret < 0 || buf[0] == '\0') {
        /* Fallback: use a deterministic string so the sub table still works */
        snprintf(buf, buflen, "app@%p", (void *)inst);
    }
    return 0;
}

/* ── WASM Exports ────────────────────────────────────────────────────────── */

int akira_native_msg_subscribe(wasm_exec_env_t exec_env, const char *topic)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_IPC, -EPERM);

    if (!topic || topic[0] == '\0') {
        return -EINVAL;
    }

    char caller[AKIRA_IPC_APP_NAME_MAX];
    if (resolve_caller(exec_env, caller, sizeof(caller)) < 0) {
        return -EINVAL;
    }

    return akira_ipc_subscribe(topic, caller);
}

int akira_native_msg_unsubscribe(wasm_exec_env_t exec_env, const char *topic)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_IPC, -EPERM);

    if (!topic || topic[0] == '\0') {
        return -EINVAL;
    }

    char caller[AKIRA_IPC_APP_NAME_MAX];
    if (resolve_caller(exec_env, caller, sizeof(caller)) < 0) {
        return -EINVAL;
    }

    return akira_ipc_unsubscribe(topic, caller);
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

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return -EINVAL;
    }

    const void *ptr = wasm_runtime_addr_app_to_native(module_inst, data_ptr);
    if (!ptr) {
        return -EFAULT;
    }

    return akira_ipc_publish(topic, ptr, len);
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

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return -EINVAL;
    }

    void *ptr = wasm_runtime_addr_app_to_native(module_inst, buf_ptr);
    if (!ptr) {
        return -EFAULT;
    }

    char caller[AKIRA_IPC_APP_NAME_MAX];
    if (resolve_caller(exec_env, caller, sizeof(caller)) < 0) {
        return -EINVAL;
    }

    k_timeout_t to = (timeout_ms < 0) ? K_FOREVER :
                     (timeout_ms == 0)  ? K_NO_WAIT  :
                                          K_MSEC(timeout_ms);

    return akira_ipc_recv(topic, caller, ptr, buf_len, to);
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

    char caller[AKIRA_IPC_APP_NAME_MAX];
    if (resolve_caller(exec_env, caller, sizeof(caller)) < 0) {
        return -EINVAL;
    }

    return akira_ipc_pending(topic, caller);
}
