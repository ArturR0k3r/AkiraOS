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
        shell_print(sh, "Battery:        unavailable (no working gauge)");
    }
    /* Comes from the charger IC, so it is valid even with no fuel gauge. */
    shell_print(sh, "Charging:       %s", akira_pm_is_charging() ? "yes" : "no");
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
#ifdef CONFIG_AKIRA_SETTINGS
    if (argc < 2)
    {
        char sv[16] = "";
        if (!akira_settings_get("akira/power/sleep_s", sv, sizeof(sv)) && sv[0])
        {
            int t = atoi(sv);
            if (t > 0)
            {
                shell_print(sh, "Deep sleep delay: %d s (NVS)", t);
            }
            else
            {
                shell_print(sh, "Deep sleep: disabled (NVS akira/power/sleep_s=0)");
            }
        }
        else
        {
            shell_print(sh, "Deep sleep delay: %d s (build default)",
                        CONFIG_AKIRA_DEEP_SLEEP_IDLE_S);
        }
        shell_print(sh, "  0 = never sleep; wake from deep sleep is a reboot.");
        shell_print(sh, "  Never sleeps while charging.");
        return 0;
    }

    int s = atoi(argv[1]);
    if (s < 0 || s > 86400)
    {
        shell_error(sh, "delay must be 0-86400 s (0 = never sleep)");
        return -EINVAL;
    }

    char sv[16];
    snprintf(sv, sizeof(sv), "%d", s);
    if (akira_settings_set("akira/power/sleep_s", sv, 0) < 0)
    {
        shell_error(sh, "Failed to persist akira/power/sleep_s");
        return -EIO;
    }
    if (s == 0)
    {
        shell_print(sh, "Auto deep-sleep disabled");
    }
    else
    {
        shell_print(sh, "Auto deep-sleep set to %d s (active within 1 s)", s);
    }
    return 0;
#else
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_print(sh, "Deep sleep delay: %d s (compile-time; no NVS in this build)",
                CONFIG_AKIRA_DEEP_SLEEP_IDLE_S);
    return 0;
#endif /* CONFIG_AKIRA_SETTINGS */
#endif /* CONFIG_AKIRA_POWER_DEEP_SLEEP */
}

/* ------------------------------------------------------------------ */
/* power stats — CPU duty cycle, the proxy for battery draw               */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_THREAD_RUNTIME_STATS) && defined(CONFIG_SCHED_THREAD_USAGE_ALL)

/**
 * Absolute cycle counters are cumulative since boot, which tells you nothing
 * about what the device is doing *now*.  We snapshot on each call and report
 * the delta, so the usual workflow is: run `power stats` to arm it, leave the
 * device in the state you want to characterise, then run it again to read the
 * duty cycle over exactly that window.
 */
struct pm_stats_snapshot {
    uint64_t total;
    uint64_t idle;
    int64_t  uptime_ms;
    bool     valid;
};

static struct pm_stats_snapshot s_prev;

struct thread_walk_ctx {
    const struct shell *sh;
    uint64_t window;
};

static void thread_stats_cb(const struct k_thread *thread, void *user_data)
{
    struct thread_walk_ctx *ctx = user_data;
    k_thread_runtime_stats_t rt;

    if (k_thread_runtime_stats_get((struct k_thread *)thread, &rt) != 0) {
        return;
    }

    const char *name = k_thread_name_get((k_tid_t)thread);
    /* execution_cycles is cumulative since boot; without a per-thread previous
     * snapshot we can only show the boot-average share.  That is still enough
     * to rank threads, which is the point.  Sub-0.1 % threads are noise. */
    uint32_t permille = ctx->window ?
        (uint32_t)((rt.execution_cycles * 1000ULL) / ctx->window) : 0;

    if (permille == 0) {
        return;
    }

    shell_print(ctx->sh, "  %-20s %3u.%u %%",
                name ? name : "(unnamed)",
                permille / 10, permille % 10);
}

static int cmd_power_stats(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    k_thread_runtime_stats_t all;
    k_thread_runtime_stats_t idle;

    if (k_thread_runtime_stats_all_get(&all) != 0 ||
        k_thread_runtime_stats_cpu_get(0, &idle) != 0) {
        shell_error(sh, "runtime stats unavailable");
        return -ENOTSUP;
    }

    /* all.execution_cycles  = cycles spent in *any* thread, idle included
     * idle.idle_cycles      = the subset spent in the idle thread          */
    uint64_t total_now = all.execution_cycles;
    uint64_t idle_now  = all.idle_cycles;
    int64_t  now_ms    = k_uptime_get();

    shell_print(sh, "=== AkiraOS Power Stats ===");

    if (!s_prev.valid) {
        shell_print(sh, "Baseline armed. Leave the device in the state you want");
        shell_print(sh, "to measure, then run 'power stats' again.");
    } else {
        uint64_t d_total = total_now - s_prev.total;
        uint64_t d_idle  = idle_now  - s_prev.idle;
        int64_t  d_ms    = now_ms    - s_prev.uptime_ms;

        if (d_total == 0) {
            shell_print(sh, "No cycles elapsed since last sample.");
        } else {
            if (d_idle > d_total) {
                d_idle = d_total;  /* counters sampled non-atomically */
            }
            /* Permille to keep this integer-only — the shell has no FPU printf
             * on this build and a float here would drag in the soft-float
             * formatter for no benefit. */
            uint32_t busy_permille =
                (uint32_t)(((d_total - d_idle) * 1000ULL) / d_total);

            shell_print(sh, "Window:         %lld ms", (long long)d_ms);
            shell_print(sh, "CPU busy:       %u.%u %%",
                        busy_permille / 10, busy_permille % 10);
            shell_print(sh, "CPU idle:       %u.%u %%",
                        (1000 - busy_permille) / 10,
                        (1000 - busy_permille) % 10);
            shell_print(sh, "  (lower busy %% = longer battery life; this is the");
            shell_print(sh, "   headline number to compare before/after a change)");
        }

        shell_print(sh, "Thread share since boot (>0.1 %%):");
        struct thread_walk_ctx ctx = { .sh = sh, .window = total_now };
        k_thread_foreach(thread_stats_cb, &ctx);
    }

    s_prev.total     = total_now;
    s_prev.idle      = idle_now;
    s_prev.uptime_ms = now_ms;
    s_prev.valid     = true;

    return 0;
}

#else /* runtime stats not compiled in */

static int cmd_power_stats(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_error(sh, "Needs CONFIG_THREAD_RUNTIME_STATS=y and "
                    "CONFIG_SCHED_THREAD_USAGE_ALL=y");
    return -ENOTSUP;
}

#endif

/* ------------------------------------------------------------------ */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_power,
    SHELL_CMD(status,  NULL, "Show power state and settings", cmd_power_status),
    SHELL_CMD(stats,   NULL, "CPU duty cycle since last call (battery proxy)", cmd_power_stats),
    SHELL_CMD(timeout, NULL, "Get/set idle timeout: power timeout [<s>]", cmd_power_timeout),
    SHELL_CMD(deep,    NULL, "Get/set auto deep-sleep delay: power deep [<s>]", cmd_power_deep),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(power, &sub_power, "AkiraOS power management", NULL);
