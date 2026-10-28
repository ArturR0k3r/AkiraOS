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
 *   power battery         — battery monitor snapshot + raw gauge/charger registers
 *   power sim <pct> <st>  — fake battery readings for UI testing (BATTERY_SIM)
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
#ifdef CONFIG_FUEL_GAUGE_STC3115
#include <zephyr/device.h>
#include "drivers/power/stc3115.h"
#endif
#ifdef CONFIG_CHARGER_BQ25601
#include <zephyr/device.h>
#include "drivers/charger/bq25601.h"
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

#ifdef CONFIG_AKIRA_POWER_MANAGER
static const char *batt_state_name(akira_batt_state_t st)
{
    switch (st) {
    case AKIRA_BATT_DISCHARGING:  return "discharging";
    case AKIRA_BATT_CHARGING:     return "charging";
    case AKIRA_BATT_FULL:         return "full";
    case AKIRA_BATT_NOT_CHARGING: return "plugged in, not charging";
    default:                      return "unknown";
    }
}
#endif

/* The first thing to run on hardware when the battery % looks wrong: shows
 * what the UI sees, then the chips' own registers so a stopped gauge, a
 * latched fault or an unbound charger is visible at a glance. */
static int cmd_power_battery(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

#ifdef CONFIG_AKIRA_POWER_MANAGER
    akira_battery_snapshot_t b;

    akira_pm_battery_snapshot(&b);
    shell_print(sh, "--- monitor (what the UI shows) ---");
    shell_print(sh, "valid=%d  %u%%  %s  external power=%s",
                b.valid, b.pct, batt_state_name(b.state), b.online ? "yes" : "no");
    shell_print(sh, "%d mV  %d mA  seq=%u", (int)b.voltage_mv, (int)b.current_ma,
                (unsigned)b.seq);
#endif

#ifdef CONFIG_FUEL_GAUGE_STC3115
    {
        const struct device *fg = DEVICE_DT_GET_ANY(st_stc3115);
        struct stc3115_regs r;

        shell_print(sh, "--- STC3115 fuel gauge ---");
        if (!fg || !device_is_ready(fg)) {
            shell_error(sh, "not ready");
        } else if (stc3115_read_regs(fg, &r) != 0) {
            shell_error(sh, "I2C read failed");
        } else {
            shell_print(sh, "ID=0x%02x present=%d restarts=%u", r.id, r.present,
                        (unsigned)r.restarts);
            shell_print(sh, "MODE=0x%02x  GG_RUN=%d VMODE=%d ALM_ENA=%d",
                        r.mode, !!(r.mode & BIT(4)), !!(r.mode & BIT(0)),
                        !!(r.mode & BIT(3)));
            shell_print(sh, "CTRL=0x%02x  PORDET=%d BATFAIL=%d GG_VM=%d IO0=%d",
                        r.ctrl, !!(r.ctrl & BIT(4)), !!(r.ctrl & BIT(3)),
                        !!(r.ctrl & BIT(2)), !!(r.ctrl & BIT(0)));
            shell_print(sh, "SOC=%u.%02u%% (raw %u)  COUNTER=%u",
                        r.soc_raw / 512, (r.soc_raw % 512) * 100 / 512,
                        r.soc_raw, r.counter);
            shell_print(sh, "V=%d mV  I=%d mA  OCV=%d mV  T=%d C",
                        (int)r.voltage_mv, (int)r.current_ma, (int)r.ocv_mv,
                        r.temperature_c);
            shell_print(sh, "CC_CNF=%u (want %u)  VM_CNF=%u (want %u)",
                        r.cc_cnf, r.cc_cnf_want, r.vm_cnf, r.vm_cnf_want);
            shell_print(sh, "RAM backup: %s, SOC %u%%",
                        r.ram_valid ? "valid" : "invalid", r.ram_soc_raw / 512);
        }
    }
#endif

#ifdef CONFIG_CHARGER_BQ25601
    {
        const struct device *chg = DEVICE_DT_GET_ANY(ti_bq25601);
        uint8_t r[BQ25601_NUM_REGS];
        static const char *const vbus[] = {
            "none", "USB SDP", "USB CDP", "USB DCP", "?", "?", "?", "OTG",
        };
        static const char *const chrg[] = {
            "not charging", "pre-charge", "fast charge", "done",
        };

        shell_print(sh, "--- BQ25601 charger ---");
        if (!chg || !device_is_ready(chg)) {
            shell_error(sh, "not ready (driver failed to bind)");
        } else if (bq25601_read_regs(chg, r) != 0) {
            shell_error(sh, "I2C read failed");
        } else {
            shell_print(sh, "REG00..0B: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                        r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9],
                        r[10], r[11]);
            shell_print(sh, "VBUS=%s  CHRG=%s  PG=%d  THERM=%d",
                        vbus[r[8] >> 5], chrg[(r[8] >> 3) & 3], !!(r[8] & BIT(2)),
                        !!(r[8] & BIT(1)));
            shell_print(sh, "faults=0x%02x (WDT=%d CHRG=%d BAT=%d NTC=%d)", r[9],
                        !!(r[9] & BIT(7)), (r[9] >> 4) & 3, !!(r[9] & BIT(3)),
                        r[9] & 7);
            shell_print(sh, "HIZ=%d CHG_EN=%d  IINDPM=%u mA  ICHG=%u mA  VREG=%u mV",
                        !!(r[0] & BIT(7)), !!(r[1] & BIT(4)),
                        100U + (r[0] & 0x1F) * 100U, (r[2] & 0x3F) * 60U,
                        3856U + (r[4] >> 3) * 32U);
            shell_print(sh, "EN_TERM=%d  watchdog=%s  PN=0x%x (BQ25601 = 0x2)",
                        !!(r[5] & BIT(7)), ((r[5] >> 4) & 3) ? "ON" : "off",
                        (r[11] >> 3) & 0xF);
        }
    }
#endif
    return 0;
}

static int cmd_power_sim(const struct shell *sh, size_t argc, char **argv)
{
#ifdef CONFIG_AKIRA_POWER_MANAGER
    if (argc < 2 || strcmp(argv[1], "off") == 0) {
        akira_pm_battery_simulate(-1, AKIRA_BATT_UNKNOWN);
        shell_print(sh, "Battery simulation off");
        return 0;
    }

    int pct = atoi(argv[1]);
    const char *st = (argc > 2) ? argv[2] : "dis";
    akira_batt_state_t state;

    if (strcmp(st, "chg") == 0) {
        state = AKIRA_BATT_CHARGING;
    } else if (strcmp(st, "full") == 0) {
        state = AKIRA_BATT_FULL;
        pct = 100;
    } else if (strcmp(st, "nochg") == 0) {
        state = AKIRA_BATT_NOT_CHARGING;
    } else {
        state = AKIRA_BATT_DISCHARGING;
    }
    if (akira_pm_battery_simulate(pct, state) != 0) {
        shell_error(sh, "Needs CONFIG_AKIRA_BATTERY_SIM=y");
        return -ENOTSUP;
    }
    shell_print(sh, "Simulating %d%% %s", pct, batt_state_name(state));
    return 0;
#else
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_error(sh, "Power manager disabled");
    return -ENOTSUP;
#endif
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_power,
    SHELL_CMD(status,  NULL, "Show power state and settings", cmd_power_status),
    SHELL_CMD(stats,   NULL, "CPU duty cycle since last call (battery proxy)", cmd_power_stats),
    SHELL_CMD(timeout, NULL, "Get/set idle timeout: power timeout [<s>]", cmd_power_timeout),
    SHELL_CMD(deep,    NULL, "Get/set auto deep-sleep delay: power deep [<s>]", cmd_power_deep),
    SHELL_CMD(battery, NULL, "Battery monitor + raw fuel gauge / charger registers", cmd_power_battery),
    SHELL_CMD(sim,     NULL, "Fake battery: power sim <pct> <chg|dis|full|nochg> | off", cmd_power_sim),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(power, &sub_power, "AkiraOS power management", NULL);
