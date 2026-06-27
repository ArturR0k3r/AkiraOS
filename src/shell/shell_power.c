/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file shell_power.c
 * @brief Shell commands for AkiraOS power management.
 *
 * Commands:
 *   power status          — show battery, current idle timeout, deep-sleep config
 *   power timeout <s>     — set idle-to-sleep timeout in seconds (0 = disable)
 *   power deep <s>        — set Phase-2 deep-sleep delay in seconds (0 = disable)
 *
 * All settings are persisted to NVS and take effect on the next idle cycle.
 * The running shell thread re-reads timeout from NVS when settings change,
 * so "power timeout 10" takes effect within one shell tick (~1 s).
 */

#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_AKIRA_SETTINGS
#include "settings/settings.h"
#endif

#ifdef CONFIG_AKIRA_POWER_MANAGER
#include "drivers/power/power_manager.h"
#endif

/* ------------------------------------------------------------------ */

static int cmd_power_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "=== AkiraOS Power Status ===");

    /* Battery */
#ifdef CONFIG_AKIRA_POWER_MANAGER
    uint8_t pct = 0;
    if (akira_pm_get_battery_level(&pct) == 0)
    {
        shell_print(sh, "Battery:        %u %%", (unsigned)pct);
    }
    else
    {
        shell_print(sh, "Battery:        unavailable");
    }
#else
    shell_print(sh, "Battery:        (power manager disabled)");
#endif

    /* Idle timeout */
#ifdef CONFIG_AKIRA_SETTINGS
    {
        char sv[16] = "";
        bool en = true;

        if (!akira_settings_get("akira/display/timeout_en", sv, sizeof(sv)))
        {
            en = (atoi(sv) != 0);
        }
        memset(sv, 0, sizeof(sv));
        akira_settings_get("akira/display/timeout_s", sv, sizeof(sv));

        if (!en)
        {
            shell_print(sh, "Idle timeout:   disabled");
        }
        else if (sv[0])
        {
            shell_print(sh, "Idle timeout:   %s s (NVS)", sv);
        }
        else
        {
            shell_print(sh, "Idle timeout:   60 s (default)");
        }
    }
#else
    shell_print(sh, "Idle timeout:   60 s (compile default, NVS unavailable)");
#endif

    /* Deep sleep */
#ifdef CONFIG_AKIRA_POWER_DEEP_SLEEP
    shell_print(sh, "Deep sleep:     enabled — delay %d s",
                CONFIG_AKIRA_DEEP_SLEEP_IDLE_S);
#else
    shell_print(sh, "Deep sleep:     disabled on this build");
#endif

    shell_print(sh, "Phase-1 bright: "
#if defined(CONFIG_LS0XX)
                "0 %% (Sharp reflective — backlight off)"
#else
                "10 %%"
#endif
    );

    return 0;
}

static int cmd_power_timeout(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2)
    {
        /* Read-back current value */
        cmd_power_status(sh, 0, NULL);
        return 0;
    }

    int s = atoi(argv[1]);
    if (s < 0 || s > 3600)
    {
        shell_error(sh, "timeout must be 0-3600 s (0 = disable)");
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_SETTINGS
    char sv[16];
    if (s == 0)
    {
        akira_settings_set("akira/display/timeout_en", "0", 0);
        shell_print(sh, "Idle timeout disabled");
    }
    else
    {
        snprintf(sv, sizeof(sv), "%d", s);
        akira_settings_set("akira/display/timeout_en", "1", 0);
        akira_settings_set("akira/display/timeout_s", sv, 0);
        shell_print(sh, "Idle timeout set to %d s (active within 1 s, no reboot needed)", s);
    }
#else
    shell_error(sh, "NVS settings not available — recompile with CONFIG_AKIRA_SETTINGS=y");
    return -ENOTSUP;
#endif
    return 0;
}

static int cmd_power_deep(const struct shell *sh, size_t argc, char **argv)
{
#ifndef CONFIG_AKIRA_POWER_DEEP_SLEEP
    shell_error(sh, "Deep sleep not enabled on this build "
                    "(CONFIG_AKIRA_POWER_DEEP_SLEEP=n)");
    return -ENOTSUP;
#else
    if (argc < 2)
    {
        shell_print(sh, "Deep sleep delay: %d s", CONFIG_AKIRA_DEEP_SLEEP_IDLE_S);
        shell_print(sh, "  (compile-time; set at build with "
                        "CONFIG_AKIRA_DEEP_SLEEP_IDLE_S=<s>)");
        return 0;
    }

    shell_print(sh, "Deep sleep delay is compile-time only "
                    "(CONFIG_AKIRA_DEEP_SLEEP_IDLE_S=%d).",
                CONFIG_AKIRA_DEEP_SLEEP_IDLE_S);
    shell_print(sh, "To change: set CONFIG_AKIRA_DEEP_SLEEP_IDLE_S in board conf and rebuild.");
    return 0;
#endif
}

/* ------------------------------------------------------------------ */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_power,
    SHELL_CMD(status,  NULL, "Show power state and settings", cmd_power_status),
    SHELL_CMD(timeout, NULL, "Get/set idle timeout: power timeout [<s>]", cmd_power_timeout),
    SHELL_CMD(deep,    NULL, "Show deep-sleep delay",  cmd_power_deep),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(power, &sub_power, "AkiraOS power management", NULL);
