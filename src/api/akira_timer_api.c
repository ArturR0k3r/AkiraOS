/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_timer
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_timer, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_timer_api.c
 * @brief Polling timer pool backed by k_uptime_get()
 */

#include "akira_timer_api.h"
#include <runtime/security.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <errno.h>
#include <string.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME

#ifndef CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES
#define CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES 8
#endif

/* -------------------------------------------------------------------------- */
/* Timer pool                                                                  */
/* -------------------------------------------------------------------------- */

struct akira_timer_slot {
    bool        allocated;
    bool        running;
    int64_t     start_ms;   /* k_uptime_get() at last start */
    int64_t     elapsed_ms; /* accumulated ms when stopped  */
    const void *owner;      /* wasm_module_inst_t of creator */
};

static struct akira_timer_slot s_pool[CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES];
static K_MUTEX_DEFINE(s_pool_lock);

int akira_timer_init(void)
{
    memset(s_pool, 0, sizeof(s_pool));
    return 0;
}

SYS_INIT(akira_timer_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static int slot_validate(wasm_exec_env_t exec_env, int32_t handle,
                         struct akira_timer_slot **out)
{
    if (handle < 0 || handle >= CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES) {
        return -EINVAL;
    }
    struct akira_timer_slot *s = &s_pool[handle];
    if (!s->allocated) {
        return -EINVAL;
    }
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (s->owner != (const void *)inst) {
        LOG_WRN("timer %d: cross-app access denied", handle);
        return -EPERM;
    }
    *out = s;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* WASM native exports                                                         */
/* -------------------------------------------------------------------------- */

int akira_native_timer_create(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_TIMER, -EPERM);

    k_mutex_lock(&s_pool_lock, K_FOREVER);

    for (int i = 0; i < CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES; i++) {
        if (!s_pool[i].allocated) {
            s_pool[i].allocated  = true;
            s_pool[i].running    = false;
            s_pool[i].start_ms   = 0;
            s_pool[i].elapsed_ms = 0;
            s_pool[i].owner = (const void *)wasm_runtime_get_module_inst(exec_env);
            k_mutex_unlock(&s_pool_lock);
            LOG_DBG("timer_create: handle=%d", i);
            return i;
        }
    }

    k_mutex_unlock(&s_pool_lock);
    LOG_ERR("timer_create: pool exhausted (max=%d)", CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES);
    return -ENOMEM;
}

int akira_native_timer_start(wasm_exec_env_t exec_env, int32_t handle)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_TIMER, -EPERM);

    struct akira_timer_slot *s;
    int ret;

    k_mutex_lock(&s_pool_lock, K_FOREVER);
    ret = slot_validate(exec_env, handle, &s);
    if (ret == 0) {
        s->start_ms   = k_uptime_get();
        s->elapsed_ms = 0;
        s->running    = true;
        LOG_DBG("timer_start: handle=%d", handle);
    }
    k_mutex_unlock(&s_pool_lock);
    return ret;
}

int akira_native_timer_stop(wasm_exec_env_t exec_env, int32_t handle)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_TIMER, -EPERM);

    struct akira_timer_slot *s;
    int ret;

    k_mutex_lock(&s_pool_lock, K_FOREVER);
    ret = slot_validate(exec_env, handle, &s);
    if (ret == 0 && s->running) {
        s->elapsed_ms = k_uptime_get() - s->start_ms;
        s->running    = false;
        LOG_DBG("timer_stop: handle=%d elapsed=%lldms", handle, s->elapsed_ms);
    }
    k_mutex_unlock(&s_pool_lock);
    return ret;
}

int akira_native_timer_elapsed(wasm_exec_env_t exec_env, int32_t handle)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_TIMER, -EPERM);

    struct akira_timer_slot *s;
    int ret;
    int64_t ms = 0;

    k_mutex_lock(&s_pool_lock, K_FOREVER);
    ret = slot_validate(exec_env, handle, &s);
    if (ret == 0) {
        ms = s->running ? (k_uptime_get() - s->start_ms) : s->elapsed_ms;
    }
    k_mutex_unlock(&s_pool_lock);

    if (ret != 0) {
        return ret;
    }
    /* Cast to int32_t: wraps after ~24.8 days — acceptable for embedded use */
    return (int32_t)(ms & 0x7FFFFFFF);
}

int akira_native_timer_free(wasm_exec_env_t exec_env, int32_t handle)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_TIMER, -EPERM);

    struct akira_timer_slot *s;
    int ret;

    k_mutex_lock(&s_pool_lock, K_FOREVER);
    ret = slot_validate(exec_env, handle, &s);
    if (ret == 0) {
        memset(s, 0, sizeof(*s));
        LOG_DBG("timer_free: handle=%d", handle);
    }
    k_mutex_unlock(&s_pool_lock);
    return ret;
}

#else /* !CONFIG_AKIRA_WASM_RUNTIME */

int akira_timer_init(void) { return 0; }

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
