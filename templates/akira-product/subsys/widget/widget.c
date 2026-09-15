/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Example product subsystem showing the three AkiraOS extension points a
 * product uses. None of this required editing AkiraOS.
 */

#include <zephyr/logging/log.h>
#include <akira_capability.h>
#include <akira_native_registry.h>
#include <akira_hooks.h>
#include <runtime/security.h>

LOG_MODULE_REGISTER(widget, LOG_LEVEL_INF);

/* 1) A product capability in the vendor range (bits 48-63). An app requests it
 *    in its manifest as "widget.actuate"; a duplicate bit fails to link. */
#define WIDGET_CAP_ACTUATE BIT64(48)
AKIRA_CAPABILITY_DEFINE(widget_actuate, "widget.actuate", 48, AKIRA_CAPABILITY_PRIVILEGED);

/* 2) A native the product exports to WASM apps, gated on that capability. */
static int widget_native_actuate(wasm_exec_env_t exec_env, int32_t position)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, WIDGET_CAP_ACTUATE, -EPERM);
    LOG_INF("widget actuate -> %d", position);
    /* drive the hardware here */
    return 0;
}

static const NativeSymbol widget_natives[] = {
    { "widget_actuate", (void *)widget_native_actuate, "(i)i", NULL },
};
AKIRA_NATIVE_API_DEFINE(widget_api, "env", widget_natives);

/* 3) A hook: bring the product up once AkiraOS is ready, instead of guessing a
 *    SYS_INIT priority or editing the boot sequence. */
static void widget_on_event(const struct akira_hook_event *e, void *user)
{
    ARG_UNUSED(user);
    if (e->type == AKIRA_HOOK_BOOT_READY) {
        LOG_INF("widget: AkiraOS ready, starting product services");
        /* start threads, open the BT service, etc. */
    }
}
AKIRA_HOOK_DEFINE(widget_boot, AKIRA_HOOK_MASK(AKIRA_HOOK_BOOT_READY),
                  widget_on_event, NULL);
