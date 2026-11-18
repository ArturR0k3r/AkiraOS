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
#include <zephyr/spinlock.h>
#include <errno.h>
#include <string.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME

#include <akira_app_containment.h>
#include <lib/mem_helper.h>

#ifndef CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES
#define CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES 8
#endif

/* One app may hold at most this many handles, so no app can take the whole
 * pool from the others. */
#define TIMER_MAX_PER_APP MAX(1, CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES / 2)

/* -------------------------------------------------------------------------- */
/* Timer pool                                                                  */
/* -------------------------------------------------------------------------- */

/* Polling timers only: no k_timer and no callbacks into WASM, so nothing can
 * fire after an app is gone. Handles are owned by the app slot (the module
 * instance pointer is reused by the next app in that slot, so it is not a
 * safe owner) and freed by the release hook below. */
struct akira_timer_slot {
    bool        allocated;
    bool        running;
    int8_t      owner_slot; /* akira_runtime_slot_of_exec_env() of creator */
    int64_t     start_ms;   /* k_uptime_get() at last start */
    int64_t     elapsed_ms; /* accumulated ms when stopped  */
};

/* Guarded by a spinlock, not a mutex: every section is a few loads and
 * stores, so an app thread can never be stopped while holding it, and the
 * release hook never waits on a lock a dead app took. */
static struct akira_timer_slot AKIRA_BULK_BSS s_pool[CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES];
static struct k_spinlock s_pool_lock;

int akira_timer_init(void)
{
    k_spinlock_key_t key = k_spin_lock(&s_pool_lock);
    memset(s_pool, 0, sizeof(s_pool));
    k_spin_unlock(&s_pool_lock, key);
    return 0;
}

SYS_INIT(akira_timer_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

/* Call with s_pool_lock held. */
static int slot_validate(int app_slot, int32_t handle, struct akira_timer_slot **out)
{
    if (handle < 0 || handle >= CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES) {
        return -EINVAL;
    }
    struct akira_timer_slot *s = &s_pool[handle];
    if (!s->allocated) {
        return -EINVAL;
    }
    if (app_slot < 0 || s->owner_slot != app_slot) {
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

    int app_slot = akira_runtime_slot_of_exec_env(exec_env);
    if (app_slot < 0) {
        return -EPERM;
    }

    int handle = -ENOMEM;
    int owned = 0;

    k_spinlock_key_t key = k_spin_lock(&s_pool_lock);
    for (int i = 0; i < CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES; i++) {
        if (s_pool[i].allocated && s_pool[i].owner_slot == app_slot) {
            owned++;
        }
    }
    if (owned < TIMER_MAX_PER_APP) {
        for (int i = 0; i < CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES; i++) {
            if (!s_pool[i].allocated) {
                s_pool[i].allocated  = true;
                s_pool[i].running    = false;
                s_pool[i].start_ms   = 0;
                s_pool[i].elapsed_ms = 0;
                s_pool[i].owner_slot = (int8_t)app_slot;
                handle = i;
                break;
            }
        }
    }
    k_spin_unlock(&s_pool_lock, key);

    if (handle < 0) {
        /* In immediate log mode the line is written under the console lock. */
        akira_native_critical_enter(exec_env);
        LOG_WRN_RATELIMIT_RATE(1000, "timer_create: no handle (app holds %d, max %d per app, pool %d)",
                               owned, TIMER_MAX_PER_APP, CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES);
        akira_native_critical_exit(exec_env);
    }
    return handle;
}

int akira_native_timer_start(wasm_exec_env_t exec_env, int32_t handle)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_TIMER, -EPERM);

    int app_slot = akira_runtime_slot_of_exec_env(exec_env);
    struct akira_timer_slot *s;
    int ret;

    k_spinlock_key_t key = k_spin_lock(&s_pool_lock);
    ret = slot_validate(app_slot, handle, &s);
    if (ret == 0) {
        s->start_ms   = k_uptime_get();
        s->elapsed_ms = 0;
        s->running    = true;
    }
    k_spin_unlock(&s_pool_lock, key);
    return ret;
}

int akira_native_timer_stop(wasm_exec_env_t exec_env, int32_t handle)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_TIMER, -EPERM);

    int app_slot = akira_runtime_slot_of_exec_env(exec_env);
    struct akira_timer_slot *s;
    int ret;

    k_spinlock_key_t key = k_spin_lock(&s_pool_lock);
    ret = slot_validate(app_slot, handle, &s);
    if (ret == 0 && s->running) {
        s->elapsed_ms = k_uptime_get() - s->start_ms;
        s->running    = false;
    }
    k_spin_unlock(&s_pool_lock, key);
    return ret;
}

int akira_native_timer_elapsed(wasm_exec_env_t exec_env, int32_t handle)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_TIMER, -EPERM);

    int app_slot = akira_runtime_slot_of_exec_env(exec_env);
    struct akira_timer_slot *s;
    int ret;
    int64_t ms = 0;

    k_spinlock_key_t key = k_spin_lock(&s_pool_lock);
    ret = slot_validate(app_slot, handle, &s);
    if (ret == 0) {
        ms = s->running ? (k_uptime_get() - s->start_ms) : s->elapsed_ms;
    }
    k_spin_unlock(&s_pool_lock, key);

    if (ret != 0) {
        return ret;
    }
    /* Cast to int32_t: wraps after ~24.8 days — acceptable for embedded use */
    return (int32_t)(ms & 0x7FFFFFFF);
}

int akira_native_timer_free(wasm_exec_env_t exec_env, int32_t handle)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_TIMER, -EPERM);

    int app_slot = akira_runtime_slot_of_exec_env(exec_env);
    struct akira_timer_slot *s;
    int ret;

    k_spinlock_key_t key = k_spin_lock(&s_pool_lock);
    ret = slot_validate(app_slot, handle, &s);
    if (ret == 0) {
        memset(s, 0, sizeof(*s));
    }
    k_spin_unlock(&s_pool_lock, key);
    return ret;
}

/* -------------------------------------------------------------------------- */
/* App teardown                                                                */
/* -------------------------------------------------------------------------- */

/* Free every handle the app left allocated, however it ended. */
static void timer_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);
    if (slot < 0) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&s_pool_lock);
    for (int i = 0; i < CONFIG_AKIRA_WASM_TIMER_MAX_HANDLES; i++) {
        if (s_pool[i].allocated && s_pool[i].owner_slot == slot) {
            memset(&s_pool[i], 0, sizeof(s_pool[i]));
        }
    }
    k_spin_unlock(&s_pool_lock, key);
}

AKIRA_APP_RESOURCE_DEFINE(timer, 50, NULL, timer_release);

#else /* !CONFIG_AKIRA_WASM_RUNTIME */

int akira_timer_init(void) { return 0; }

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_TIMER))
#include <akira_native_registry.h>

static const NativeSymbol akira_timer_natives[] = {
    {"timer_create", (void *)akira_native_timer_create, "()i", NULL},
    {"timer_start", (void *)akira_native_timer_start, "(i)i", NULL},
    {"timer_stop", (void *)akira_native_timer_stop, "(i)i", NULL},
    {"timer_elapsed", (void *)akira_native_timer_elapsed, "(i)i", NULL},
    {"timer_free", (void *)akira_native_timer_free, "(i)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_timer_api, "env", akira_timer_natives);
#endif
