/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
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
#include "akira_common_api.h"
#include "akira_wasm_mem.h"
#include <runtime/security.h>
#include <runtime/akira_runtime.h>
#include <akira_app_containment.h>
#include <lib/mem_helper.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/spinlock.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef CONFIG_AKIRA_APP_MANAGER
#include <runtime/app_manager/app_manager.h>
#endif

#ifdef CONFIG_AKIRA_CLOUD_APP_HANDLER
#include <connectivity/cloud/cloud_app_handler.h>
#endif

/* APP_NAME_MAX_LEN defined in app_manager.h; provide fallback for non-manager builds */
#ifndef APP_NAME_MAX_LEN
#define APP_NAME_MAX_LEN 32
#endif

/* strnlen() without relying on POSIX visibility (native_sim's host libc
 * hides it under -std=c99). */
static size_t bounded_strlen(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

/* App names fit APP_NAME_MAX_LEN with their NUL. */
static bool name_ok(const char *name)
{
    return name && name[0] != '\0' &&
           bounded_strlen(name, APP_NAME_MAX_LEN) < APP_NAME_MAX_LEN;
}

/* True if @p name is the calling app itself. */
static bool is_self(wasm_exec_env_t exec_env, const char *name)
{
    char self[APP_NAME_MAX_LEN];

    return akira_app_self_name(exec_env, self, sizeof(self)) == 0 &&
           strncmp(self, name, sizeof(self)) == 0;
}

#ifdef CONFIG_AKIRA_APP_MANAGER

/* ── Lifecycle work queue ────────────────────────────────────────────────
 *
 * app_manager_start() and app_manager_stop() never run on a WASM app's
 * thread:
 *  - app_manager_start() reads the target binary from LittleFS (flash),
 *    which temporarily disables the SPI0 PSRAM cache on ESP32-S3, and holds
 *    the registry mutex across the flash read and the module load;
 *  - app_manager_stop() waits up to the stop timeout for the target and may
 *    abort it and run its teardown.
 * An app stopped or aborted in the middle of either would leak the registry
 * lock, and an app blocked in one keeps HOME waiting for it. They also must
 * not run on the system work queue: the chain is too deep for its stack,
 * and a stop that waits there would hold up the crash cleanup it waits for.
 *
 * So they run here, on a dedicated queue. 4 KB: a start loads the module
 * with WAMR's full bytecode validation (no mini loader), and a stop that
 * has to abort an unresponsive app runs the slot's teardown (file closes,
 * registry save) on this stack. 2 KB was only enough for the mini loader.
 */
#define SWITCH_WQ_STACK_SIZE 4096
static K_THREAD_STACK_DEFINE(g_switch_wq_stack, SWITCH_WQ_STACK_SIZE);
static struct k_work_q        g_switch_wq;

/* Guards g_switch_target and g_lc_reqs / g_lc_rate. */
static struct k_spinlock      g_lc_lock;

/* ── Deferred app_switch ─────────────────────────────────────────────────
 *  1. app_switch() copies the target name, schedules a k_work_delayable
 *     item, and returns 0 immediately.
 *  2. The WASM thread returns from main(), exits; slot-cleanup work frees
 *     its SRAM stack (submitted to sysworkq, lightweight).
 *  3. 200 ms later the switch work fires on the lifecycle queue and calls
 *     app_manager_start() — WASM thread gone, SPI0 / PSRAM idle.
 */
static char                   g_switch_target[APP_NAME_MAX_LEN];
static struct k_work_delayable g_switch_work;

static void switch_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    char target[APP_NAME_MAX_LEN];

    k_spinlock_key_t key = k_spin_lock(&g_lc_lock);
    memcpy(target, g_switch_target, sizeof(target));
    target[sizeof(target) - 1] = '\0';
    g_switch_target[0] = '\0';
    k_spin_unlock(&g_lc_lock, key);

    if (target[0] == '\0') {
        return;
    }
    LOG_INF("app_switch: starting '%s' (deferred)", target);
    int ret = app_manager_start(target);
    if (ret < 0) {
        LOG_ERR("app_switch: deferred start of '%s' failed: %d", target, ret);
    }
}

/* ── Deferred calls with a result (app_start, app_stop, ...) ─────────────
 *
 * The request is copied into a small static table and run in order on the
 * lifecycle queue; the calling app waits for the result in short,
 * stop-aware sleeps. If the app is stopped (or gives up after
 * LC_WAIT_MAX_MS) the request still completes and frees its entry itself;
 * no pointer into the app is kept. */
#define LC_REQ_DEPTH    4
#define LC_POLL_MS      20
#define LC_WAIT_MAX_MS  60000

enum { LC_FREE = 0, LC_QUEUED, LC_RUNNING, LC_DONE };

struct lc_req {
    akira_lifecycle_op_t op;
    uint32_t seq;
    int result;
    int8_t waiter_slot; /* app slot waiting for the result, -1 once abandoned */
    uint8_t state;
    char name[APP_NAME_MAX_LEN];
};

static struct lc_req AKIRA_BULK_BSS g_lc_reqs[LC_REQ_DEPTH];
static uint32_t g_lc_seq;

/* Restart storms: each app slot may queue LC_RATE_BURST lifecycle requests
 * at once and one more every LC_RATE_PERIOD_MS after that, so no app can
 * keep the queue (and the flash) busy loading and stopping apps. */
#define LC_RATE_BURST     4
#define LC_RATE_PERIOD_MS 2000

struct lc_rate {
    int64_t last_ms;
    uint8_t tokens;
    bool primed;
};

static struct lc_rate AKIRA_BULK_BSS g_lc_rate[AKIRA_MAX_WASM_INSTANCES];

static void lc_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    for (;;) {
        struct lc_req *req = NULL;
        akira_lifecycle_op_t op = NULL;
        char name[APP_NAME_MAX_LEN];

        k_spinlock_key_t key = k_spin_lock(&g_lc_lock);
        for (int i = 0; i < LC_REQ_DEPTH; i++) {
            struct lc_req *r = &g_lc_reqs[i];

            if (r->state == LC_QUEUED &&
                (!req || (int32_t)(r->seq - req->seq) < 0)) {
                req = r;
            }
        }
        if (req) {
            req->state = LC_RUNNING;
            op = req->op;
            memcpy(name, req->name, sizeof(name));
            name[sizeof(name) - 1] = '\0';
        }
        k_spin_unlock(&g_lc_lock, key);

        if (!req) {
            return;
        }

        int ret = op ? op(name) : -EINVAL;

        key = k_spin_lock(&g_lc_lock);
        req->result = ret;
        if (req->waiter_slot >= 0) {
            req->state = LC_DONE;
        } else {
            req->state = LC_FREE; /* the caller gave up waiting */
        }
        k_spin_unlock(&g_lc_lock, key);

        if (ret < 0) {
            LOG_WRN("deferred lifecycle call for '%s' failed: %d", name, ret);
        }
    }
}

static K_WORK_DEFINE(g_lc_work, lc_work_fn);

static bool lc_rate_take(int slot)
{
    struct lc_rate *r = &g_lc_rate[slot];
    int64_t now = k_uptime_get();
    bool ok;

    k_spinlock_key_t key = k_spin_lock(&g_lc_lock);
    if (!r->primed) {
        r->primed = true;
        r->tokens = LC_RATE_BURST;
        r->last_ms = now;
    }
    int64_t refill = (now - r->last_ms) / LC_RATE_PERIOD_MS;
    if (refill > 0) {
        r->tokens = (uint8_t)MIN((int64_t)r->tokens + refill, (int64_t)LC_RATE_BURST);
        r->last_ms += refill * LC_RATE_PERIOD_MS;
    }
    ok = r->tokens > 0;
    if (ok) {
        r->tokens--;
    }
    k_spin_unlock(&g_lc_lock, key);
    return ok;
}

int akira_lifecycle_run_deferred(wasm_exec_env_t exec_env, akira_lifecycle_op_t op,
                                 const char *name)
{
    int slot = akira_runtime_slot_of_exec_env(exec_env);

    if (!op || !name_ok(name)) {
        return -EINVAL;
    }
    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        return -EPERM;
    }
    if (!lc_rate_take(slot)) {
        /* In immediate log mode the line is written under the console lock. */
        akira_native_critical_enter(exec_env);
        LOG_WRN_RATELIMIT_RATE(2000, "lifecycle: app slot %d is sending requests too fast", slot);
        akira_native_critical_exit(exec_env);
        return -EAGAIN;
    }

    struct lc_req *req = NULL;
    k_spinlock_key_t key = k_spin_lock(&g_lc_lock);
    for (int i = 0; i < LC_REQ_DEPTH; i++) {
        if (g_lc_reqs[i].state == LC_FREE) {
            req = &g_lc_reqs[i];
            req->op = op;
            req->seq = ++g_lc_seq;
            req->result = 0;
            req->waiter_slot = (int8_t)slot;
            strncpy(req->name, name, sizeof(req->name) - 1);
            req->name[sizeof(req->name) - 1] = '\0';
            req->state = LC_QUEUED;
            break;
        }
    }
    k_spin_unlock(&g_lc_lock, key);

    if (!req) {
        return -EBUSY; /* LC_REQ_DEPTH requests already pending */
    }
    (void)k_work_submit_to_queue(&g_switch_wq, &g_lc_work);

    int64_t deadline = k_uptime_get() + LC_WAIT_MAX_MS;

    for (;;) {
        bool stop = akira_runtime_stop_requested(exec_env);
        bool late = k_uptime_get() >= deadline;
        int ret = 0;
        bool done;

        key = k_spin_lock(&g_lc_lock);
        done = req->state == LC_DONE;
        if (done) {
            ret = req->result;
            req->state = LC_FREE;
            req->waiter_slot = -1;
        } else if (stop || late) {
            req->waiter_slot = -1; /* the worker frees the entry when done */
        }
        k_spin_unlock(&g_lc_lock, key);

        if (done) {
            return ret;
        }
        if (stop) {
            return -EINTR;
        }
        if (late) {
            return -ETIMEDOUT; /* still queued or running */
        }
        (void)akira_runtime_sleep(exec_env, K_MSEC(LC_POLL_MS));
    }
}

/* The app is gone: drop its claim on pending results (an aborted waiter
 * never got to), and give the next app in the slot a fresh request budget. */
static void lifecycle_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);
    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&g_lc_lock);
    for (int i = 0; i < LC_REQ_DEPTH; i++) {
        struct lc_req *r = &g_lc_reqs[i];

        if (r->state != LC_FREE && r->waiter_slot == slot) {
            r->waiter_slot = -1;
            if (r->state == LC_DONE) {
                r->state = LC_FREE;
            }
        }
    }
    g_lc_rate[slot].primed = false;
    k_spin_unlock(&g_lc_lock, key);
}

AKIRA_APP_RESOURCE_DEFINE(lifecycle, 50, NULL, lifecycle_release);

static int lifecycle_wq_init(void)
{
    struct k_work_queue_config cfg = {
        .name      = "switch_wq",
        .no_yield  = false,
    };

    k_work_init_delayable(&g_switch_work, switch_work_fn);
    for (int i = 0; i < LC_REQ_DEPTH; i++) {
        g_lc_reqs[i].state = LC_FREE;
        g_lc_reqs[i].waiter_slot = -1;
    }
    k_work_queue_start(&g_switch_wq, g_switch_wq_stack,
                       K_THREAD_STACK_SIZEOF(g_switch_wq_stack),
                       CONFIG_AKIRA_WASM_APP_PRIORITY, &cfg);
    return 0;
}

/* Started once at boot, so two apps can never race to start it. */
SYS_INIT(lifecycle_wq_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#else /* !CONFIG_AKIRA_APP_MANAGER */

int akira_lifecycle_run_deferred(wasm_exec_env_t exec_env, akira_lifecycle_op_t op,
                                 const char *name)
{
    ARG_UNUSED(exec_env);
    ARG_UNUSED(op);
    ARG_UNUSED(name);
    return -ENOTSUP;
}

#endif /* CONFIG_AKIRA_APP_MANAGER */

/* ── WASM Exports ────────────────────────────────────────────────────────── */

int akira_native_app_get_status(wasm_exec_env_t exec_env, const char *name)
{
    /* Read-only query: app.info is sufficient; app.control also accepted */
    uint64_t mask = akira_security_get_cap_mask(exec_env);
    if (!(mask & (AKIRA_CAP_APP_INFO | AKIRA_CAP_APP_CONTROL))) {
        AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_INFO, -EPERM);
    }

    if (!name || name[0] == '\0') {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    /* Takes the registry lock: no abort inside it. */
    akira_native_critical_enter(exec_env);
    int state = (int)app_manager_get_state(name);
    akira_native_critical_exit(exec_env);
    return state;
#else
    return -ENOTSUP;
#endif
}

int akira_native_app_list(wasm_exec_env_t exec_env,
                          uint32_t buf_ptr, uint32_t buf_len)
{
    /* Read-only query: app.info is sufficient; app.control also accepted */
    uint64_t mask = akira_security_get_cap_mask(exec_env);
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

    /* Validates the whole [buf_ptr, buf_ptr + buf_len) range. */
    char *wasm_buf = (char *)akira_wasm_ptr_to_native(exec_env, buf_ptr, buf_len);
    if (!wasm_buf) {
        return -EFAULT;
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    /* Heap-allocated: a full app_info_t listing is larger than the app thread
     * stack. The registry lock is taken inside: no abort there. */
    int count;
    akira_native_critical_enter(exec_env);
    app_info_t *apps = app_manager_list_alloc(&count);
    akira_native_critical_exit(exec_env);
    if (!apps) {
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
    akira_free_buffer(apps);
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
    uint64_t mask = akira_security_get_cap_mask(exec_env);
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

    char *wasm_buf = (char *)akira_wasm_ptr_to_native(exec_env, buf_ptr, buf_len);
    if (!wasm_buf) {
        return -EFAULT;
    }

    char name[APP_NAME_MAX_LEN] = {0};
    int ret = akira_app_self_name(exec_env, name, sizeof(name));
    if (ret < 0) {
        wasm_buf[0] = '\0';
        return ret;
    }

    size_t nlen = bounded_strlen(name, sizeof(name));
    size_t copy = (nlen < buf_len - 1) ? nlen : buf_len - 1;
    memcpy(wasm_buf, name, copy);
    wasm_buf[copy] = '\0';
    return (int)copy;
}

/* ── Write-control API (requires AKIRA_CAP_APP_CONTROL) ─────────────────── */

int akira_native_app_start(wasm_exec_env_t exec_env, const char *name)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_CONTROL, -EPERM);

    if (!name_ok(name)) {
        LOG_DBG("app_start: invalid name");
        return -EINVAL;
    }

    /* Self-start prevention (recursive instantiation) */
    if (is_self(exec_env, name)) {
        LOG_DBG("app_start: cannot start self (%s)", name);
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    /* Runs on the lifecycle queue; this waits for the result (stop-aware). */
    return akira_lifecycle_run_deferred(exec_env, app_manager_start, name);
#else
    return -ENOTSUP;
#endif
}

int akira_native_app_stop(wasm_exec_env_t exec_env, const char *name)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_CONTROL, -EPERM);

    if (!name_ok(name)) {
        LOG_DBG("app_stop: invalid name");
        return -EINVAL;
    }

    /* Prevent an app from stopping itself via API — use return 0 from main().
     * Stopping itself synchronously would wait on its own thread. */
    if (is_self(exec_env, name)) {
        LOG_DBG("app_stop: cannot stop self (%s) — return 0 from main instead", name);
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    /* Runs on the lifecycle queue; this waits for the result (stop-aware). */
    return akira_lifecycle_run_deferred(exec_env, app_manager_stop, name);
#else
    return -ENOTSUP;
#endif
}

int akira_native_app_switch(wasm_exec_env_t exec_env, const char *name)
{
    /* Both app.switch and app.control grant access to this lighter operation */
    uint64_t mask = akira_security_get_cap_mask(exec_env);
    if (!(mask & (AKIRA_CAP_APP_SWITCH | AKIRA_CAP_APP_CONTROL))) {
        /* Reports (rate-limited) and returns -EPERM. */
        AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_SWITCH, -EPERM);
    }

    if (!name_ok(name)) {
        LOG_DBG("app_switch: invalid target name");
        return -EINVAL;
    }

    /* Prevent switching to self */
    if (is_self(exec_env, name)) {
        LOG_DBG("app_switch: cannot switch to self (%s)", name);
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        return -EPERM;
    }
    if (!lc_rate_take(slot)) {
        return -EAGAIN;
    }

    /* Cancel any pending (unstarted) switch — last call wins */
    k_work_cancel_delayable(&g_switch_work);

    k_spinlock_key_t key = k_spin_lock(&g_lc_lock);
    strncpy(g_switch_target, name, APP_NAME_MAX_LEN - 1);
    g_switch_target[APP_NAME_MAX_LEN - 1] = '\0';
    k_spin_unlock(&g_lc_lock, key);

    k_work_reschedule_for_queue(&g_switch_wq, &g_switch_work, K_MSEC(200));
    return 0;
#else
    return -ENOTSUP;
#endif
}

/* Registered "(*~)i": WAMR validated [ver_buf, ver_buf + buf_len) and passes
 * a native pointer. */
int akira_native_app_check_update(wasm_exec_env_t exec_env, uint8_t *ver_buf, uint32_t buf_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_CONTROL, -EPERM);

    if (!ver_buf || buf_len == 0) {
        return -EINVAL;
    }

    char self[APP_NAME_MAX_LEN];
    if (akira_app_self_name(exec_env, self, sizeof(self)) < 0) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    /* app_info_t is ~600 bytes: not on the app thread's stack. */
    app_info_t *info = akira_malloc_buffer(sizeof(*info));
    if (!info) {
        return -ENOMEM;
    }
    akira_native_critical_enter(exec_env); /* registry lock inside */
    int ret = app_manager_get_info(self, info);
    akira_native_critical_exit(exec_env);
    if (ret == 0) {
        ret = info->has_update ? 1 : 0;
    }
    if (ret == 1) {
        size_t vlen = bounded_strlen(info->available_version, APP_VERSION_MAX_LEN);
        size_t copy = (vlen < buf_len - 1) ? vlen : buf_len - 1;
        memcpy(ver_buf, info->available_version, copy);
        ver_buf[copy] = '\0';
    }
    akira_free_buffer(info);
    return ret;
#else
    return -ENOTSUP;
#endif
}

int akira_native_app_request_update(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_APP_CONTROL, -EPERM);

    char self[APP_NAME_MAX_LEN];
    if (akira_app_self_name(exec_env, self, sizeof(self)) < 0) {
        return -EINVAL;
    }

#if defined(CONFIG_AKIRA_APP_MANAGER) && defined(CONFIG_AKIRA_CLOUD_APP_HANDLER)
    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        return -EPERM;
    }
    if (!lc_rate_take(slot)) {
        return -EAGAIN;
    }

    app_info_t *info = akira_malloc_buffer(sizeof(*info));
    if (!info) {
        return -ENOMEM;
    }
    akira_native_critical_enter(exec_env); /* registry / handler locks inside */
    int ret = app_manager_get_info(self, info);
    if (ret == 0 && !info->has_update) {
        ret = -ENOENT;
    }
    /* cloud_app_update() returns quickly (queues async download on the cloud
     * client's own thread) — safe to call synchronously from WASM native
     * context, unlike app_manager_start()'s flash/PSRAM path (see the
     * lifecycle work queue above for why that one differs). */
    if (ret == 0) {
        ret = cloud_app_update(self, NULL, NULL);
    }
    akira_native_critical_exit(exec_env);
    akira_free_buffer(info);
    return ret;
#else
    return -ENOTSUP;
#endif
}

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_LIFECYCLE))
#include <akira_native_registry.h>

static const NativeSymbol akira_lifecycle_natives[] = {
    {"app_get_status", (void *)akira_native_app_get_status, "($)i", NULL},
    {"app_list", (void *)akira_native_app_list, "(ii)i", NULL},
    {"app_get_self_name", (void *)akira_native_app_get_self_name, "(ii)i", NULL},
    /* Write-control — requires AKIRA_CAP_APP_CONTROL */
    {"app_start", (void *)akira_native_app_start, "($)i", NULL},
    {"app_stop", (void *)akira_native_app_stop, "($)i", NULL},
    /* Lightweight handoff — requires AKIRA_CAP_APP_SWITCH or APP_CONTROL */
    {"app_switch", (void *)akira_native_app_switch, "($)i", NULL},
    /* "*~": the version buffer is validated by WAMR for its full length (the
     * WASM type, (i32, i32) -> i32, is unchanged). */
    {"app_check_update", (void *)akira_native_app_check_update, "(*~)i", NULL},
    {"app_request_update", (void *)akira_native_app_request_update, "()i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_lifecycle_api, "env", akira_lifecycle_natives);
#endif
