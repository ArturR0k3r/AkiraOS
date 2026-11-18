/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_display_own
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_display_own, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file display_ownership.c
 * @brief Display ownership arbitration between the OS shell and WASM apps.
 */

#include "display_ownership.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <api/akira_display_api.h>

/* Mutex protecting g_owner transitions */
static K_MUTEX_DEFINE(g_display_mutex);

/* Current owner — shell owns the display at boot.  Atomic: the raw-write
 * handshake in akira_display_api.c relies on "owner store, then busy load"
 * here racing "busy CAS, then owner load" there being sequentially
 * consistent. */
static atomic_t g_owner = ATOMIC_INIT(DISPLAY_OWNER_SHELL);

/* ------------------------------------------------------------------ */

int akira_display_claim_shell(void)
{
    int ret = k_mutex_lock(&g_display_mutex, K_MSEC(500));

    if (ret < 0) {
        LOG_ERR("Display claim timed out: current owner=%d", (int)atomic_get(&g_owner));
        return ret;
    }

    /* Owner first: from here on the app's draw and raw-write calls get
     * -EBUSY, so nothing new can be queued behind the wait below. */
    atomic_set(&g_owner, DISPLAY_OWNER_SHELL);
    k_mutex_unlock(&g_display_mutex);

#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_DISPLAY)
    /* Let an in-flight app transfer finish before the shell draws.  Bounded:
     * an app aborted mid-transfer cannot freeze the shell here. */
    akira_display_raw_async_wait();
#endif

    LOG_INF("Display claimed by OS shell");
    return 0;
}

void akira_display_release_to_wasm(void)
{
    k_mutex_lock(&g_display_mutex, K_FOREVER);

    atomic_set(&g_owner, DISPLAY_OWNER_WASM);
    k_mutex_unlock(&g_display_mutex);

    LOG_INF("Display released to WASM app");
}

display_owner_t akira_display_get_owner(void)
{
    return (display_owner_t)atomic_get(&g_owner);
}
