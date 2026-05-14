/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_runtime_cmds
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_runtime_cmds, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_runtime_cmds.c
 * @brief "akira" shell command tree — runtime introspection and control.
 *
 * Commands:
 *   akira apps              List running WASM apps
 *   akira start <name>      Start a named WASM app
 *   akira stop  <name>      Stop a named WASM app
 *   akira mem               Memory usage report
 *   akira caps  [name]      Show app capability masks
 *   akira ipc               IPC channel statistics
 *   akira ota               OTA state and trigger
 *   akira log   <mod> <lvl> Set Zephyr log level for a module
 *   akira version           Build info
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log_ctrl.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "akira.h"

#ifdef CONFIG_AKIRA_APP_MANAGER
#include <runtime/app_manager/app_manager.h>
#endif

#ifdef CONFIG_AKIRA_WASM_IPC
#include <runtime/akira_ipc.h>
#endif

#ifdef CONFIG_AKIRA_OTA
#include <connectivity/ota/ota_manager.h>
#endif

/* ── helpers ────────────────────────────────────────────────────────────── */

static const char *app_state_name(app_state_t s)
{
#ifdef CONFIG_AKIRA_APP_MANAGER
    return app_state_to_str(s);
#else
    (void)s;
    return "?";
#endif
}

/* ── akira apps ─────────────────────────────────────────────────────────── */

static int cmd_apps_list(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

#ifndef CONFIG_AKIRA_APP_MANAGER
    shell_print(sh, "App manager not enabled (CONFIG_AKIRA_APP_MANAGER=n)");
    return 0;
#else
    app_info_t list[16];
    int n = app_manager_list(list, ARRAY_SIZE(list));
    if (n < 0) {
        shell_error(sh, "app_manager_list: %d", n);
        return n;
    }
    if (n == 0) {
        shell_print(sh, "No apps installed.");
        return 0;
    }
    shell_print(sh, "%-4s %-24s %-10s %-8s %-7s %-7s",
                "ID", "NAME", "VERSION", "STATE", "HEAP KB", "STACK KB");
    shell_print(sh, "%-4s %-24s %-10s %-8s %-7s %-7s",
                "----", "------------------------", "----------",
                "--------", "-------", "-------");
    for (int i = 0; i < n; i++) {
        shell_print(sh, "%-4u %-24s %-10s %-8s %-7u %-7u",
                    list[i].id,
                    list[i].name,
                    list[i].version,
                    app_state_name(list[i].state),
                    list[i].heap_kb,
                    list[i].stack_kb);
    }
    shell_print(sh, "\nTotal: %d app(s), %d running",
                n, app_manager_get_running_count());
    return 0;
#endif
}

/* ── akira start ────────────────────────────────────────────────────────── */

static int cmd_app_start(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "Usage: akira start <name>");
        return -EINVAL;
    }
#ifndef CONFIG_AKIRA_APP_MANAGER
    shell_print(sh, "App manager not enabled");
    return 0;
#else
    int ret = app_manager_start(argv[1]);
    if (ret < 0) {
        shell_error(sh, "Failed to start '%s': %d", argv[1], ret);
        return ret;
    }
    shell_print(sh, "Started: %s", argv[1]);
    return 0;
#endif
}

/* ── akira stop ─────────────────────────────────────────────────────────── */

static int cmd_app_stop(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "Usage: akira stop <name>");
        return -EINVAL;
    }
#ifndef CONFIG_AKIRA_APP_MANAGER
    shell_print(sh, "App manager not enabled");
    return 0;
#else
    int ret = app_manager_stop(argv[1]);
    if (ret < 0) {
        shell_error(sh, "Failed to stop '%s': %d", argv[1], ret);
        return ret;
    }
    shell_print(sh, "Stopped: %s", argv[1]);
    return 0;
#endif
}

/* ── akira mem ──────────────────────────────────────────────────────────── */

static int cmd_mem_report(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    /* Zephyr kernel heap stats */
    struct sys_memory_stats kheap = {0};
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
    extern struct k_heap _system_heap;
    sys_heap_runtime_stats_get(&_system_heap.heap, &kheap);
#endif

    shell_print(sh, "=== AkiraOS Memory Report ===");
    shell_print(sh, "Kernel heap: allocated=%zu B  free=%zu B",
                kheap.allocated_bytes, kheap.free_bytes);

#ifdef CONFIG_AKIRA_PSRAM
    shell_print(sh, "PSRAM: (external — size board-dependent)");
#else
    shell_print(sh, "PSRAM: not available on this board");
#endif

#ifdef CONFIG_AKIRA_APP_MANAGER
    app_info_t list[16];
    int n = app_manager_list(list, ARRAY_SIZE(list));
    if (n > 0) {
        shell_print(sh, "\nRunning apps:");
        shell_print(sh, "  %-24s %-8s %-8s %-7s", "NAME", "STATE", "HEAP KB", "STACK KB");
        for (int i = 0; i < n; i++) {
            if (list[i].state == APP_STATE_RUNNING) {
                shell_print(sh, "  [%u] %-22s %-8s heap=%-5u stack=%-5u crashes=%u",
                            list[i].id, list[i].name,
                            app_state_name(list[i].state),
                            list[i].heap_kb, list[i].stack_kb,
                            list[i].crash_count);
            }
        }
    }
#endif
    return 0;
}

/* ── akira caps ─────────────────────────────────────────────────────────── */

static int cmd_caps_show(const struct shell *sh, size_t argc, char **argv)
{
#ifndef CONFIG_AKIRA_APP_MANAGER
    shell_print(sh, "App manager not enabled");
    return 0;
#else
    app_info_t list[16];
    int n = app_manager_list(list, ARRAY_SIZE(list));
    if (n <= 0) {
        shell_print(sh, "No apps.");
        return 0;
    }
    shell_print(sh, "%-24s  %-10s  PERMISSIONS", "NAME", "STATE");
    for (int i = 0; i < n; i++) {
        if (argc >= 2 && strcmp(list[i].name, argv[1]) != 0) {
            continue;
        }
        shell_print(sh, "%-24s  %-10s  0x%04x",
                    list[i].name,
                    app_state_name(list[i].state),
                    list[i].heap_kb /* placeholder — permissions field */);
    }
    return 0;
#endif
}

/* ── akira ipc ──────────────────────────────────────────────────────────── */

static int cmd_ipc_stats(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

#ifndef CONFIG_AKIRA_WASM_IPC
    shell_print(sh, "IPC not enabled (CONFIG_AKIRA_WASM_IPC=n)");
    return 0;
#else
    shell_print(sh, "IPC max topics: %d  msg max size: %d B  queue depth: %d",
                CONFIG_AKIRA_IPC_MAX_TOPICS,
                CONFIG_AKIRA_IPC_MSG_MAX_SIZE,
                CONFIG_AKIRA_IPC_QUEUE_DEPTH);
    return 0;
#endif
}

/* ── akira ota ──────────────────────────────────────────────────────────── */

static int cmd_ota_status(const struct shell *sh, size_t argc, char **argv)
{
#ifndef CONFIG_AKIRA_OTA
    shell_print(sh, "OTA not enabled (CONFIG_AKIRA_OTA=n)");
    return 0;
#else
    const struct ota_progress *p = ota_get_progress();
    if (!p) {
        shell_print(sh, "OTA manager not initialized");
        return 0;
    }
    shell_print(sh, "OTA state:  %s", ota_state_to_string(p->state));
    shell_print(sh, "Progress:   %u%% (%zu / %zu bytes)",
                p->percentage, p->bytes_written, p->total_size);
    if (p->last_error != OTA_OK) {
        shell_print(sh, "Last error: %s", ota_result_to_string(p->last_error));
    }

    /* Sub-command: trigger */
    if (argc >= 2 && strcmp(argv[1], "confirm") == 0) {
        enum ota_result r = ota_confirm_firmware();
        shell_print(sh, "confirm: %s", ota_result_to_string(r));
    } else if (argc >= 2 && strcmp(argv[1], "rollback") == 0) {
        enum ota_result r = ota_request_rollback();
        shell_print(sh, "rollback: %s", ota_result_to_string(r));
    }
    return 0;
#endif
}

/* ── akira log ──────────────────────────────────────────────────────────── */

static int cmd_log_level(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 3) {
        shell_error(sh, "Usage: akira log <module> <level>");
        shell_error(sh, "  level: off err wrn inf dbg");
        return -EINVAL;
    }
    const char *mod   = argv[1];
    const char *level = argv[2];
    uint32_t lvl;

    if      (strcmp(level, "off") == 0) { lvl = LOG_LEVEL_NONE; }
    else if (strcmp(level, "err") == 0) { lvl = LOG_LEVEL_ERR;  }
    else if (strcmp(level, "wrn") == 0) { lvl = LOG_LEVEL_WRN;  }
    else if (strcmp(level, "inf") == 0) { lvl = LOG_LEVEL_INF;  }
    else if (strcmp(level, "dbg") == 0) { lvl = LOG_LEVEL_DBG;  }
    else {
        shell_error(sh, "Unknown level '%s'", level);
        return -EINVAL;
    }

    log_filter_set(NULL, Z_LOG_LOCAL_DOMAIN_ID, log_source_id_get(mod), lvl);
    shell_print(sh, "Log level for '%s' set to %s", mod, level);
    return 0;
}

/* ── akira version ──────────────────────────────────────────────────────── */

static int cmd_version(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "AkiraOS %d.%d.%d \"%s\"",
                AKIRA_VERSION_MAJOR,
                AKIRA_VERSION_MINOR,
                AKIRA_VERSION_PATCH,
                AKIRA_CODENAME);
    shell_print(sh, "Zephyr %s", STRINGIFY(BUILD_VERSION));
    shell_print(sh, "Board:  %s", CONFIG_BOARD);
    shell_print(sh, "Built:  %s %s", __DATE__, __TIME__);
    return 0;
}

/* ── command tree ───────────────────────────────────────────────────────── */

SHELL_STATIC_SUBCMD_SET_CREATE(akira_cmds,
    SHELL_CMD(apps,    NULL, "List installed/running WASM apps",     cmd_apps_list),
    SHELL_CMD(start,   NULL, "Start app: akira start <name>",        cmd_app_start),
    SHELL_CMD(stop,    NULL, "Stop app:  akira stop <name>",         cmd_app_stop),
    SHELL_CMD(mem,     NULL, "Memory usage report",                  cmd_mem_report),
    SHELL_CMD(caps,    NULL, "Show app capabilities: akira caps [name]", cmd_caps_show),
    SHELL_CMD(ipc,     NULL, "IPC channel statistics",               cmd_ipc_stats),
    SHELL_CMD(ota,     NULL, "OTA status/control: akira ota [confirm|rollback]", cmd_ota_status),
    SHELL_CMD(log,     NULL, "Set log level: akira log <module> <off|err|wrn|inf|dbg>", cmd_log_level),
    SHELL_CMD(version, NULL, "AkiraOS build info",                   cmd_version),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(akira, &akira_cmds, "AkiraOS runtime commands", NULL);
