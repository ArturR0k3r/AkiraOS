/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_lifecycle_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_lifecycle_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_lifecycle_api.c
 * @brief App Lifecycle WASM native API
 *
 * Routes all calls through app_manager_* (never directly through
 * akira_runtime_*) so the persistent registry and crash counters
 * are always kept consistent.
 *
 * Self-start prevention: if the caller and the target share the same
 * name, the call is rejected with -EINVAL to prevent recursive instantiation.
 */

#include "akira_lifecycle_api.h"
#include <runtime/security.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef CONFIG_AKIRA_APP_MANAGER
#include <runtime/app_manager/app_manager.h>
#endif

/* APP_NAME_MAX_LEN defined in app_manager.h; provide fallback for non-manager builds */
#ifndef APP_NAME_MAX_LEN
#define APP_NAME_MAX_LEN 32
#endif

/* ── Deferred app_switch ─────────────────────────────────────────────────
 *
 * app_switch() must NOT call app_manager_start() synchronously from inside
 * the WASM native call context.  app_manager_start() reads the target WASM
 * binary from LittleFS (flash), which temporarily disables the SPI0 PSRAM
 * cache on ESP32-S3.  Any concurrent PSRAM access (WiFi DMA, display DMA,
 * other threads with PSRAM buffers) during that window causes a hard fault.
 *
 * We also must NOT run it on the system work queue: app_manager_start()
 * calls LittleFS, manifest parser, and akira_runtime_start — this chain
 * easily exceeds sysworkq's default 1 KB stack → stack overflow / fatal.
 *
 * Solution: dedicated work queue with its own 4 KB SRAM stack.
 *   1. app_switch() copies the target name, schedules a k_work_delayable
 *      item, and returns 0 immediately.
 *   2. The WASM thread returns from main(), exits; slot-cleanup work frees
 *      its SRAM stack (submitted to sysworkq, lightweight).
 *   3. 200 ms later the switch work fires on the dedicated queue and calls
 *      app_manager_start() safely — WASM thread gone, SPI0 / PSRAM idle.
 */
/* 2 KB: this work queue only dispatches akira_runtime_start/stop; no deep
 * call chains. 4 KB was double what the work items actually need. */
#define SWITCH_WQ_STACK_SIZE 2048
static K_THREAD_STACK_DEFINE(g_switch_wq_stack, SWITCH_WQ_STACK_SIZE);
static struct k_work_q        g_switch_wq;
static bool                   g_switch_wq_started;

static char                   g_switch_target[APP_NAME_MAX_LEN];
static struct k_work_delayable g_switch_work;

static void switch_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    if (g_switch_target[0] == '\0') {
        return;
    }
    LOG_INF("app_switch: starting '%s' (deferred)", g_switch_target);
    int ret = app_manager_start(g_switch_target);
    if (ret < 0) {
        LOG_ERR("app_switch: deferred start of '%s' failed: %d",
                g_switch_target, ret);
    }
    g_switch_target[0] = '\0';
}

static void ensure_switch_wq_init(void)
{
    if (g_switch_wq_started) {
        return;
    }
    k_work_init_delayable(&g_switch_work, switch_work_fn);
    struct k_work_queue_config cfg = {
        .name      = "switch_wq",
        .no_yield  = false,
    };
    k_work_queue_start(&g_switch_wq, g_switch_wq_stack,
                       K_THREAD_STACK_SIZEOF(g_switch_wq_stack),
                       CONFIG_AKIRA_WASM_APP_PRIORITY, &cfg);
    g_switch_wq_started = true;
}

/* ── WASM Exports ────────────────────────────────────────────────────────── */

int akira_native_app_get_status(wasm_exec_env_t exec_env, const char *name)
{
    /* Read-only query: app.info is sufficient; app.control also accepted */
    uint32_t mask = akira_security_get_cap_mask(exec_env);
    if (!(mask & (AKIRA_CAP_APP_INFO | AKIRA_CAP_APP_CONTROL))) {
        AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_INFO, -EPERM);
    }

    if (!name || name[0] == '\0') {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    return (int)app_manager_get_state(name);
#else
    return -ENOTSUP;
#endif
}

int akira_native_app_list(wasm_exec_env_t exec_env,
                          uint32_t buf_ptr, uint32_t buf_len)
{
    /* Read-only query: app.info is sufficient; app.control also accepted */
    uint32_t mask = akira_security_get_cap_mask(exec_env);
    if (!(mask & (AKIRA_CAP_APP_INFO | AKIRA_CAP_APP_CONTROL))) {
        AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_INFO, -EPERM);
    }

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return -EINVAL;
    }

    if (buf_len == 0) {
        return -EINVAL;
    }

    char *wasm_buf = (char *)wasm_runtime_addr_app_to_native(module_inst, buf_ptr);
    if (!wasm_buf) {
        return -EFAULT;
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    /* Retrieve all apps into a local stack buffer — avoid PSRAM alloc here */
#define LIST_MAX 16
    app_info_t apps[LIST_MAX];
    int count = app_manager_list(apps, LIST_MAX);
    if (count < 0) {
        return count;
    }

    size_t pos = 0;
    for (int i = 0; i < count && pos < buf_len - 1; i++) {
        const char *state_str = app_state_to_str(apps[i].state);
        int n = snprintf(wasm_buf + pos, buf_len - pos,
                         "%s:%s\n", apps[i].name, state_str);
        if (n < 0 || (size_t)n >= buf_len - pos) {
            break;
        }
        pos += n;
    }
    wasm_buf[pos] = '\0';
    return count;
#else
    (void)wasm_buf;
    (void)buf_len;
    return -ENOTSUP;
#endif
}

int akira_native_app_get_self_name(wasm_exec_env_t exec_env,
                                   uint32_t buf_ptr, uint32_t buf_len)
{
    /* Self-name is identity-only: app.info suffices; app.control also accepted */
    uint32_t mask = akira_security_get_cap_mask(exec_env);
    if (!(mask & (AKIRA_CAP_APP_INFO | AKIRA_CAP_APP_CONTROL))) {
        AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_INFO, -EPERM);
    }

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return -EINVAL;
    }

    if (buf_len < 2) {
        return -EINVAL;
    }

    char *wasm_buf = (char *)wasm_runtime_addr_app_to_native(module_inst, buf_ptr);
    if (!wasm_buf) {
        return -EFAULT;
    }

    char name[APP_NAME_MAX_LEN] = {0};
    int ret = akira_runtime_get_name_for_module_inst(module_inst, name, sizeof(name));
    if (ret < 0) {
        wasm_buf[0] = '\0';
        return ret;
    }

    size_t nlen = strnlen(name, sizeof(name));
    size_t copy = (nlen < buf_len - 1) ? nlen : buf_len - 1;
    memcpy(wasm_buf, name, copy);
    wasm_buf[copy] = '\0';
    return (int)copy;
}

/* ── Write-control API (requires AKIRA_CAP_APP_CONTROL) ─────────────────── */

int akira_native_app_start(wasm_exec_env_t exec_env, const char *name)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_CONTROL, -EPERM);

    if (!name || name[0] == '\0') {
        LOG_ERR("app_start: invalid name");
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    return app_manager_start(name);
#else
    return -ENOTSUP;
#endif
}

int akira_native_app_stop(wasm_exec_env_t exec_env, const char *name)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_CONTROL, -EPERM);

    if (!name || name[0] == '\0') {
        LOG_ERR("app_stop: invalid name");
        return -EINVAL;
    }

    /* Prevent an app from stopping itself via API — use return 0 from main() */
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (inst) {
        char self[APP_NAME_MAX_LEN] = {0};
        if (akira_runtime_get_name_for_module_inst(inst, self, sizeof(self)) == 0 &&
            self[0] != '\0' && strcmp(self, name) == 0) {
            LOG_WRN("app_stop: cannot stop self (%s) — return 0 from main instead", name);
            return -EINVAL;
        }
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    return app_manager_stop(name);
#else
    return -ENOTSUP;
#endif
}

int akira_native_app_switch(wasm_exec_env_t exec_env, const char *name)
{
    /* Both app.switch and app.control grant access to this lighter operation */
    uint32_t mask = akira_security_get_cap_mask(exec_env);
    if (!(mask & (AKIRA_CAP_APP_SWITCH | AKIRA_CAP_APP_CONTROL))) {
        LOG_WRN("app_switch: capability denied (need app.switch or app.control)");
        return -EPERM;
    }

    if (!name || name[0] == '\0') {
        LOG_ERR("app_switch: invalid target name");
        return -EINVAL;
    }

    /* Prevent switching to self */
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (inst) {
        char self[APP_NAME_MAX_LEN] = {0};
        if (akira_runtime_get_name_for_module_inst(inst, self, sizeof(self)) == 0 &&
            self[0] != '\0' && strcmp(self, name) == 0) {
            LOG_WRN("app_switch: cannot switch to self (%s)", name);
            return -EINVAL;
        }
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    ensure_switch_wq_init();

    /* Cancel any pending (unstarted) switch — last call wins */
    k_work_cancel_delayable(&g_switch_work);

    strncpy(g_switch_target, name, APP_NAME_MAX_LEN - 1);
    g_switch_target[APP_NAME_MAX_LEN - 1] = '\0';

    k_work_reschedule_for_queue(&g_switch_wq, &g_switch_work, K_MSEC(200));
    return 0;
#else
    return -ENOTSUP;
#endif
}
