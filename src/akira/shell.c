/**
 * @file akira/shell.c
 * @brief AkiraOS Core Shell Commands
 *
 * Provides shell commands for interacting with and debugging
 * AkiraOS core subsystems.
 */

#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include "akira.h"
#include "kernel/psram.h"

/*===========================================================================*/
/* Shell Command Handlers                                                    */
/*===========================================================================*/

static int cmd_akira_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    akira_print_status();
    return 0;
}

static int cmd_akira_version(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "AkiraOS %s", akira_version_string());
    shell_print(sh, "Platform: %s", akira_hal_platform());
    return 0;
}

static int cmd_akira_banner(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    akira_print_banner();
    return 0;
}

static int cmd_akira_services(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    akira_service_print_all();
    return 0;
}

static int cmd_akira_processes(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    akira_process_print_table();
    return 0;
}

static int cmd_akira_timers(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    akira_timer_print_all();
    return 0;
}

static int cmd_akira_memory(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    akira_memory_dump();
    return 0;
}

static int cmd_akira_psram(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    if (!akira_psram_available()) {
        shell_print(sh, "PSRAM: Not available on this platform");
        return 0;
    }

    akira_psram_stats_t stats;
    if (akira_psram_get_stats(&stats) < 0) {
        shell_error(sh, "Failed to get PSRAM stats");
        return -EIO;
    }

    shell_print(sh, "=== PSRAM Status (ESP32-S3 N16R8) ===");
    shell_print(sh, "Total:   %zu bytes (%.2f MB)", 
                stats.total_bytes, 
                (double)stats.total_bytes / (1024.0 * 1024.0));
    shell_print(sh, "Used:    %zu bytes (%.1f%%)", 
                stats.used_bytes,
                stats.total_bytes > 0 ? 
                    (double)stats.used_bytes * 100.0 / stats.total_bytes : 0.0);
    shell_print(sh, "Free:    %zu bytes (%.2f MB)", 
                stats.free_bytes,
                (double)stats.free_bytes / (1024.0 * 1024.0));
    shell_print(sh, "Peak:    %zu bytes", stats.peak_usage);
    shell_print(sh, "Allocs:  %u", stats.alloc_count);
    shell_print(sh, "Frees:   %u", stats.free_count);
    shell_print(sh, "Failures: %u", stats.alloc_failures);

    return 0;
}

static int cmd_akira_uptime(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    uint32_t sec = akira_uptime_sec();
    uint32_t hours = sec / 3600;
    uint32_t mins = (sec % 3600) / 60;
    uint32_t secs = sec % 60;

    shell_print(sh, "Uptime: %uh %um %us", hours, mins, secs);
    return 0;
}

static int cmd_akira_reset(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "Resetting system...");
    k_msleep(500);
    akira_hal_reset();
    return 0;
}

static int cmd_akira_hal(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "=== HAL Status ===");
    shell_print(sh, "Platform: %s", akira_hal_platform());
    shell_print(sh, "HW Rev: %s", akira_hal_hw_revision());

    shell_print(sh, "Features:");
    shell_print(sh, "  GPIO: %s", akira_hal_has_feature(AKIRA_HAL_GPIO) ? "yes" : "no");
    shell_print(sh, "  SPI: %s", akira_hal_has_feature(AKIRA_HAL_SPI) ? "yes" : "no");
    shell_print(sh, "  I2C: %s", akira_hal_has_feature(AKIRA_HAL_I2C) ? "yes" : "no");
    shell_print(sh, "  WiFi: %s", akira_hal_has_feature(AKIRA_HAL_WIFI) ? "yes" : "no");
    shell_print(sh, "  BT: %s", akira_hal_has_feature(AKIRA_HAL_BT) ? "yes" : "no");
    shell_print(sh, "  Display: %s", akira_hal_has_feature(AKIRA_HAL_DISPLAY) ? "yes" : "no");

    return 0;
}

static int cmd_service_start(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_error(sh, "Usage: akira service start <name>");
        return -EINVAL;
    }

    akira_service_t *svc = akira_service_find_by_name(argv[1]);
    if (!svc)
    {
        shell_error(sh, "Service '%s' not found", argv[1]);
        return -ENOENT;
    }

    int ret = akira_service_start(svc);
    if (ret < 0)
    {
        shell_error(sh, "Failed to start service: %d", ret);
        return ret;
    }

    shell_print(sh, "Service '%s' started", argv[1]);
    return 0;
}

static int cmd_service_stop(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_error(sh, "Usage: akira service stop <name>");
        return -EINVAL;
    }

    akira_service_t *svc = akira_service_find_by_name(argv[1]);
    if (!svc)
    {
        shell_error(sh, "Service '%s' not found", argv[1]);
        return -ENOENT;
    }

    int ret = akira_service_stop(svc);
    if (ret < 0)
    {
        shell_error(sh, "Failed to stop service: %d", ret);
        return ret;
    }

    shell_print(sh, "Service '%s' stopped", argv[1]);
    return 0;
}

/*===========================================================================*/
/* Shell Command Registration                                                */
/*===========================================================================*/

SHELL_STATIC_SUBCMD_SET_CREATE(sub_service,
                               SHELL_CMD(start, NULL, "Start a service", cmd_service_start),
                               SHELL_CMD(stop, NULL, "Stop a service", cmd_service_stop),
                               SHELL_CMD(list, NULL, "List all services", cmd_akira_services),
                               SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_akira,
                               SHELL_CMD(status, NULL, "Show AkiraOS status", cmd_akira_status),
                               SHELL_CMD(version, NULL, "Show version info", cmd_akira_version),
                               SHELL_CMD(banner, NULL, "Show AkiraOS banner", cmd_akira_banner),
                               SHELL_CMD(uptime, NULL, "Show system uptime", cmd_akira_uptime),
                               SHELL_CMD(memory, NULL, "Show memory status", cmd_akira_memory),
                               SHELL_CMD(psram, NULL, "Show PSRAM status", cmd_akira_psram),
                               SHELL_CMD(services, NULL, "Show services", cmd_akira_services),
                               SHELL_CMD(processes, NULL, "Show processes", cmd_akira_processes),
                               SHELL_CMD(timers, NULL, "Show timers", cmd_akira_timers),
                               SHELL_CMD(hal, NULL, "Show HAL status", cmd_akira_hal),
                               SHELL_CMD(service, &sub_service, "Service commands", NULL),
                               SHELL_CMD(reset, NULL, "Reset the system", cmd_akira_reset),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(akira, &sub_akira, "AkiraOS commands", NULL);
