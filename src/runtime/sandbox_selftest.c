/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sandbox_selftest.c
 * @brief Test-only native that faults on purpose, to prove that a crash in
 *        an app's thread stops the app and leaves the system running.
 *
 *   int selftest_fault(int kind)
 *     0  k_oops()            an assert-style kernel oops
 *     1  write through NULL  a CPU exception (real hardware only; on
 *                            native_sim this kills the host process)
 *     2  runaway recursion   caught by the stack sentinel / MPU guard
 *     3  spin forever        in the native, not holding any lock (HOME
 *                            must still stop the app within the grace time)
 *     4  k_oops() inside a   the slot must be parked: no release hooks,
 *        critical section    never reused, system keeps running
 *
 * CONFIG_AKIRA_SANDBOX_SELFTEST only. Never ship it.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <wasm_export.h>
#include <akira_native_registry.h>
#include <akira_app_containment.h>

LOG_MODULE_REGISTER(akira_selftest, CONFIG_AKIRA_LOG_LEVEL);

static volatile int g_sink;
static volatile bool g_never; /* never set: hides the endless recursion from the compiler */

static int __noinline recurse(int depth)
{
    volatile uint8_t pad[256];

    pad[0] = (uint8_t)depth;
    g_sink += pad[0];
    if (g_never) {
        return depth;
    }
    return recurse(depth + 1) + pad[0];
}

static int32_t akira_native_selftest_fault(wasm_exec_env_t exec_env, int32_t kind)
{
    LOG_WRN("selftest_fault(%d): faulting on purpose", kind);
    switch (kind) {
    case 0:
        k_oops();
        break;
    case 1:
        *(volatile int *)0 = 0x41;
        break;
    case 2:
        return recurse(0);
    case 3:
        for (;;) {
            g_sink++;
        }
    case 4:
        akira_native_critical_enter(exec_env);
        k_oops();
        akira_native_critical_exit(exec_env);
        break;
    default:
        break;
    }
    return -EINVAL;
}

static const NativeSymbol akira_selftest_natives[] = {
    {"selftest_fault", (void *)akira_native_selftest_fault, "(i)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_selftest_api, "env", akira_selftest_natives);
