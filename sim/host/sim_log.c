/*
 * AkiraConsole Simulator — Log host
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <wasm_export.h>

/* WASM: printf_native(fmt, ...) — format string only, no varargs via WAMR */
int sim_log_printf(wasm_exec_env_t env, const char *fmt)
{
    (void)env;
    return printf("[wasm] %s", fmt);
}

/* WASM: delay(microseconds) — matches akira_api.h signature */
int sim_delay(wasm_exec_env_t env, int us)
{
    (void)env;
    struct timespec ts = {
        .tv_sec  = us / 1000000,
        .tv_nsec = (long)(us % 1000000) * 1000L,
    };
    nanosleep(&ts, NULL);
    return 0;
}
