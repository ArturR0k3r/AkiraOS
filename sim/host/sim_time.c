/*
 * AkiraConsole Simulator — Time host
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <time.h>
#include <wasm_export.h>

/* WASM: time_ms() → milliseconds since boot */
int sim_time_ms(wasm_exec_env_t env)
{
    (void)env;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* WASM: time_us() → microseconds since boot */
int sim_time_us(wasm_exec_env_t env)
{
    (void)env;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}

/* WASM: time_unix() → UNIX timestamp (seconds) */
int sim_time_unix(wasm_exec_env_t env)
{
    (void)env;
    return (int)time(NULL);
}
