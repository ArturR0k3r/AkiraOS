/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_date
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_date, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file shell_date.c
 * @brief  date shell command — set and get real wall-clock time.
 *
 * Usage:
 *   date get              → print current date/time
 *   date set 2026-04-05 14:30:00   → set date and time
 *
 * Time is tracked as an integer offset from Zephyr uptime:
 *   real_epoch = s_time_base + k_uptime_get() / 1000
 *
 * s_time_base is persisted to NVS under "system/time_base" so the
 * clock survives reboots (as long as the NVS partition is intact and
 * someone called `date set` at least once).
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/shell/shell.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <lib/akira_time.h>

#ifdef CONFIG_AKIRA_SETTINGS
#include <settings/settings.h>
#define TIME_BASE_KEY  "system/time_base"
#endif

/* Offset such that: real_epoch = s_time_base + uptime_s */
static int64_t s_time_base = 0;
static bool    s_clock_set = false;

/* ------------------------------------------------------------------ */
/* Public API (declared in include/lib/akira_time.h)                  */
/* ------------------------------------------------------------------ */

int64_t akira_time_get_epoch(void)
{
    return s_time_base + (int64_t)(k_uptime_get() / 1000);
}

void akira_time_set_epoch(int64_t epoch_s)
{
    s_time_base = epoch_s - (int64_t)(k_uptime_get() / 1000);
    s_clock_set = true;

#ifdef CONFIG_AKIRA_SETTINGS
    /* Persist base so it survives reboot */
    char buf[24];
    snprintf(buf, sizeof(buf), "%lld", (long long)s_time_base);
    akira_settings_set(TIME_BASE_KEY, buf, 0);
#endif
}

bool akira_time_is_set(void)
{
    return s_clock_set;
}

/* ------------------------------------------------------------------ */
/* Boot init: restore time_base from NVS                              */
/* ------------------------------------------------------------------ */
#ifdef CONFIG_AKIRA_SETTINGS
static int shell_date_init(void)
{
    char buf[24] = {0};
    if (akira_settings_get(TIME_BASE_KEY, buf, sizeof(buf)) == 0) {
        long long base = strtoll(buf, NULL, 10);
        if (base != 0) {
            s_time_base = (int64_t)base;
            s_clock_set = true;
            LOG_INF("Clock restored: base=%lld", (long long)s_time_base);
        }
    }
    return 0;
}
SYS_INIT(shell_date_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif

/* ------------------------------------------------------------------ */
/* Simple month-day table (non-leap-year; leap handled via year check) */
/* ------------------------------------------------------------------ */
static const int s_mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

/* Days since Unix epoch (1970-01-01) to start of year. */
static int64_t days_since_epoch(int y, int m, int d)
{
    /* Count days from 1970 to (y-1), then months, then days */
    int64_t days = 0;
    for (int yr = 1970; yr < y; yr++) {
        bool leap = (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0));
        days += leap ? 366 : 365;
    }
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (int mo = 1; mo < m; mo++) {
        days += (mo == 2 && leap) ? 29 : s_mdays[mo - 1];
    }
    days += d - 1;
    return days;
}

/* Parse "YYYY-MM-DD HH:MM:SS" and return Unix epoch; -1 on error. */
static int64_t parse_datetime(const char *date_str, const char *time_str)
{
    int y, mo, d, h, mi, s;
    if (sscanf(date_str, "%d-%d-%d", &y, &mo, &d) != 3) return -1;
    if (sscanf(time_str, "%d:%d:%d",  &h,  &mi, &s) != 3) return -1;
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return -1;
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) return -1;

    int64_t epoch = days_since_epoch(y, mo, d) * 86400LL
                  + (int64_t)h  * 3600
                  + (int64_t)mi * 60
                  + s;
    return epoch;
}

/* Format epoch as "YYYY-MM-DD HH:MM:SS" */
static void format_datetime(int64_t epoch, char *buf, size_t len)
{
    /* Convert epoch to calendar — simple Gregorian decomposition */
    int64_t days = epoch / 86400;
    int64_t rem  = epoch % 86400;
    if (rem < 0) { rem += 86400; days--; }

    int h  = (int)(rem / 3600);
    int mi = (int)((rem % 3600) / 60);
    int s  = (int)(rem % 60);

    /* Find year */
    int y = 1970;
    while (1) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        int ydays  = leap ? 366 : 365;
        if (days < ydays) break;
        days -= ydays;
        y++;
    }
    /* Find month */
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    int mo = 1;
    while (mo < 12) {
        int md = (mo == 2 && leap) ? 29 : s_mdays[mo - 1];
        if (days < md) break;
        days -= md;
        mo++;
    }
    int d = (int)days + 1;

    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
}

/* ------------------------------------------------------------------ */
/* Shell commands                                                      */
/* ------------------------------------------------------------------ */
static int cmd_date_get(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int64_t epoch = akira_time_get_epoch();
    char buf[24];

    if (!s_clock_set) {
        shell_print(sh, "Clock not set (showing uptime)");
    }
    format_datetime(epoch, buf, sizeof(buf));
    shell_print(sh, "%s", buf);
    return 0;
}

static int cmd_date_set(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 3) {
        shell_print(sh, "Usage: date set YYYY-MM-DD HH:MM:SS");
        return -EINVAL;
    }

    int64_t epoch = parse_datetime(argv[1], argv[2]);
    if (epoch < 0) {
        shell_print(sh, "Invalid format. Use: date set YYYY-MM-DD HH:MM:SS");
        return -EINVAL;
    }

    akira_time_set_epoch(epoch);

    char buf[24];
    format_datetime(epoch, buf, sizeof(buf));
    shell_print(sh, "Clock set: %s", buf);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(date_cmds,
    SHELL_CMD_ARG(get, NULL, "Print current date/time", cmd_date_get, 1, 0),
    SHELL_CMD_ARG(set, NULL, "[YYYY-MM-DD] [HH:MM:SS] - Set date and time",
                  cmd_date_set, 3, 0),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(date, &date_cmds, "Date/time commands", NULL);
