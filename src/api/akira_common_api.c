#include "akira_api.h"
#include "akira_common_api.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <string.h>
#include <runtime/security.h>

/* App output (printf_native) is logged at INFO; this module logs nothing else
 * at INFO, so it keeps INFO even where CONFIG_AKIRA_LOG_LEVEL is WRN. The
 * per-app output budget below bounds what apps can print. */
#if CONFIG_AKIRA_LOG_LEVEL > 3
#define APP_OUT_LOG_LEVEL CONFIG_AKIRA_LOG_LEVEL
#else
#define APP_OUT_LOG_LEVEL 3 /* LOG_LEVEL_INF */
#endif
LOG_MODULE_REGISTER(akira_common_api, APP_OUT_LOG_LEVEL);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <runtime/akira_runtime.h>
#include <akira_app_containment.h>
#include <lib/mem_helper.h>

extern akira_managed_app_t g_apps[AKIRA_MAX_WASM_INSTANCES];

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

int akira_app_self_name(wasm_exec_env_t exec_env, char *buf, size_t len)
{
    if (!buf || len == 0) {
        return -EINVAL;
    }
    buf[0] = '\0';

    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0) {
        return -ENOENT;
    }

    /* No runtime lock: a running app's own name does not change, and natives
     * called in a loop must not hold a system lock when a stop aborts them. */
    const char *name = g_apps[slot].name;
    size_t n = bounded_strlen(name, MIN(len, sizeof(g_apps[slot].name)) - 1);

    memcpy(buf, name, n);
    buf[n] = '\0';
    return n > 0 ? 0 : -ENOENT;
}

/* ── Per-app output budget (token bucket, in bytes) ──────────────────────
 * App output (printf_native, akira_log, WASI stdout/stderr) shares one
 * budget per app slot. Each line costs its length plus APP_OUT_LINE_COST for
 * the log prefix. An app may burst APP_OUT_BURST bytes and then sustain
 * APP_OUT_RATE bytes/s. Over budget, lines are dropped and counted; the next
 * line that fits reports the gap. Lines are cut at APP_OUT_LINE_MAX
 * characters. Without this an app printing in a loop keeps the console busy
 * (in immediate log mode, on its own thread, under the shell's output lock)
 * and starves the shell and watchdog-feeding work. */
#define APP_OUT_BURST      2048
#define APP_OUT_RATE       1024
#define APP_OUT_LINE_COST  32
#define APP_OUT_LINE_MAX   160

struct app_out_bucket {
    int64_t last_ms;
    int32_t tokens;
    uint32_t dropped;
    bool primed;
};

static struct app_out_bucket AKIRA_BULK_BSS g_out[AKIRA_MAX_WASM_INSTANCES];
static struct k_spinlock g_out_lock;

/* Charge @p cost bytes to @p slot. On success returns true and moves the
 * count of lines dropped since the last success into *dropped. */
static bool app_out_charge(int slot, int32_t cost, uint32_t *dropped)
{
    struct app_out_bucket *b = &g_out[slot];
    int64_t now = k_uptime_get();
    bool ok;

    k_spinlock_key_t key = k_spin_lock(&g_out_lock);
    if (!b->primed) {
        b->primed = true;
        b->tokens = APP_OUT_BURST;
        b->dropped = 0;
        b->last_ms = now;
    }
    int64_t refill = (now - b->last_ms) * APP_OUT_RATE / 1000;
    if (refill > 0) {
        b->tokens = (int32_t)MIN((int64_t)b->tokens + refill, (int64_t)APP_OUT_BURST);
        b->last_ms = now;
    }
    ok = b->tokens >= cost;
    if (ok) {
        b->tokens -= cost;
        *dropped = b->dropped;
        b->dropped = 0;
    } else if (b->dropped < UINT32_MAX) {
        b->dropped++;
    }
    k_spin_unlock(&g_out_lock, key);
    return ok;
}

static void app_out_emit(int level, const char *who, const char *text)
{
    switch (level) {
    case LOG_LEVEL_ERR:
        LOG_ERR("[%s] %s", who, text);
        break;
    case LOG_LEVEL_WRN:
        LOG_WRN("[%s] %s", who, text);
        break;
    case LOG_LEVEL_DBG:
        LOG_DBG("[%s] %s", who, text);
        break;
    default:
        LOG_INF("[%s] %s", who, text);
        break;
    }
}

/* One line of app output. @p who is resolved on first use (*who_ok). */
static void app_out_line(wasm_exec_env_t exec_env, int slot, int level,
                         char *who, size_t who_len, bool *who_ok,
                         const char *line, size_t len)
{
    uint32_t dropped = 0;

    if (len > APP_OUT_LINE_MAX) {
        len = APP_OUT_LINE_MAX;
    }
    if (slot >= 0 && slot < AKIRA_MAX_WASM_INSTANCES &&
        !app_out_charge(slot, (int32_t)len + APP_OUT_LINE_COST, &dropped)) {
        return;
    }
    if (!*who_ok) {
        if (akira_app_self_name(exec_env, who, who_len) < 0) {
            strncpy(who, "app", who_len - 1);
            who[who_len - 1] = '\0';
        }
        *who_ok = true;
    }

    /* NUL-terminated host copy: @p line is a slice of app memory. Control
     * characters are replaced so an app can't drive the console terminal
     * (escape sequences) from its output. */
    char text[APP_OUT_LINE_MAX + 1];

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)line[i];

        text[i] = (c < 0x20 && c != '\t') || c == 0x7f ? '.' : (char)c;
    }
    text[len] = '\0';

    /* In immediate log mode the backend writes the line on this thread while
     * holding the shell's output lock; a stop must not abort the app in the
     * middle of it, or the lock is lost and every later log line blocks. */
    akira_native_critical_enter(exec_env);
    if (dropped) {
        LOG_WRN("[%s] %u line(s) of output dropped (rate limit)", who, dropped);
    }
    app_out_emit(level, who, text);
    akira_native_critical_exit(exec_env);
}

void akira_app_log_write(wasm_exec_env_t exec_env, int level, const char *buf, size_t len)
{
    /* Same filter as the LOG_* macros below: compiled-out levels cost nothing. */
    if (!buf || len == 0 || level > APP_OUT_LOG_LEVEL) {
        return;
    }

    char who[32];
    bool who_ok = false;
    int slot = akira_runtime_slot_of_exec_env(exec_env);

    /* One log line per text line; "\r" and the line breaks are not logged. */
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\0') {
            if (i > start) {
                app_out_line(exec_env, slot, level, who, sizeof(who), &who_ok,
                             buf + start, i - start);
            }
            if (i < len && buf[i] == '\0') {
                break;
            }
            start = i + 1;
        }
    }
}

void akira_app_console_write(wasm_exec_env_t exec_env, const char *buf, size_t len)
{
    akira_app_log_write(exec_env, LOG_LEVEL_INF, buf, len);
}

/* A new app in this slot starts with a full budget. */
static void app_out_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);
    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&g_out_lock);
    g_out[slot].primed = false;
    k_spin_unlock(&g_out_lock, key);
}

AKIRA_APP_RESOURCE_DEFINE(app_console, 50, NULL, app_out_release);

int akira_native_printf(wasm_exec_env_t exec_env, char *message)
{
    if (!message) {
        return -EINVAL;
    }

    /* "$" validated the whole string; a few lines' worth is looked at. */
    akira_app_console_write(exec_env, message, bounded_strlen(message, 4 * APP_OUT_LINE_MAX));
    return 0;
}

int akira_native_delay(wasm_exec_env_t exec_env, uint32_t microseconds)
{
    /* No capability: timing is always allowed. The sleep ends early when the
     * app is stopped, so a stop never waits for an app-chosen delay. */
    if (microseconds == 0) {
        k_yield();
        return 0;
    }
    (void)akira_runtime_sleep(exec_env, K_USEC(microseconds));
    return 0;
}

#endif

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_API))
#include <akira_native_registry.h>

static const NativeSymbol akira_common_natives[] = {
    {"printf_native", (void *)akira_native_printf, "($)i", NULL},
    {"delay", (void *)akira_native_delay, "(i)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_common_api, "env", akira_common_natives);
#endif
