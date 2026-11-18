/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_resources.c
 * @brief Release what a WASM app left behind (akira_app_containment.h), and
 *        the default fatal-error handler that contains app faults.
 */

#include <akira_app_containment.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_app_res, CONFIG_AKIRA_LOG_LEVEL);

void akira_app_resources_on_stop_request(int slot, const char *app_name)
{
    STRUCT_SECTION_FOREACH(akira_app_resource, res) {
        if (res->on_stop) {
            res->on_stop(slot, app_name ? app_name : "");
        }
    }
}

void akira_app_resources_release(int slot, const char *app_name)
{
    /* Each hook only touches what this slot owns, so this is safe whatever
     * the app left behind, and safe to repeat. */
    STRUCT_SECTION_FOREACH(akira_app_resource, res) {
        if (res->release) {
            res->release(slot, app_name ? app_name : "");
        }
    }
}

/* ---- fatal errors --------------------------------------------------------
 * AKIRA_PANIC and the boot guard have their own handlers, which ask the
 * runtime first. Without them this is the handler: an app thread's fault is
 * contained, anything else keeps Zephyr's default policy (halt; a hardware
 * watchdog then resets). */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && !defined(CONFIG_AKIRA_PANIC) && \
    !defined(CONFIG_AKIRA_BOOT_GUARD_FATAL_REBOOT)
#include <zephyr/fatal.h>
#include <zephyr/logging/log_ctrl.h>

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    if (akira_runtime_contain_fault(reason, esf)) {
        return;
    }
    LOG_PANIC();
    LOG_ERR("Halting system");
    k_fatal_halt(reason);
}
#endif
