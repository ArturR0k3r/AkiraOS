/**
 * @file akira_shell.c
 * @brief Akira Shell Module Implementation for ESP32 Gaming Device
 *
 * Provides comprehensive system management with gaming-specific features
 */

#include "akira_shell.h"
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/stats/stats.h>
#include <zephyr/device.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(akira_shell, LOG_LEVEL_INF);

/* Thread stack and control block */
static K_THREAD_STACK_DEFINE(shell_monitor_stack, SHELL_THREAD_STACK_SIZE);
static struct k_thread shell_monitor_thread;
static k_tid_t shell_monitor_tid;

/* GPIO devices and pins based on overlay */
static const struct gpio_dt_spec button_specs[] = {0};

/* Display control GPIO specs */

/* System state */
static struct display_config current_display_config = {
    .backlight_on = true,
    .brightness = 255,
    .rotation = 0,
    .inverted = false};

static char command_history[MAX_COMMAND_HISTORY][64];
static size_t history_count = 0;
static size_t history_index = 0;

/* Command aliases */
struct command_alias
{
    char alias[16];
    char command[64];
    bool active;
};

static struct command_alias aliases[MAX_ALIAS_COUNT];
static size_t alias_count = 0;

static K_MUTEX_DEFINE(shell_state_mutex);

/* Helper functions */
static int init_gpio_pins(void)
{
    int ret;

    /* Initialize button GPIO pins */
    for (int i = 0; i < ARRAY_SIZE(button_specs); i++)
    {
        if (!gpio_is_ready_dt(&button_specs[i]))
        {
            LOG_ERR("Button %d GPIO not ready", i);
            return -ENODEV;
        }

        ret = gpio_pin_configure_dt(&button_specs[i], GPIO_INPUT);
        if (ret)
        {
            LOG_ERR("Failed to configure button %d: %d", i, ret);
            return ret;
        }
    }

    LOG_INF("GPIO pins initialized successfully");
    return 0;
}

static void add_to_history(const char *command)
{
    k_mutex_lock(&shell_state_mutex, K_FOREVER);

    strncpy(command_history[history_index], command, sizeof(command_history[0]) - 1);
    command_history[history_index][sizeof(command_history[0]) - 1] = '\0';

    history_index = (history_index + 1) % MAX_COMMAND_HISTORY;
    if (history_count < MAX_COMMAND_HISTORY)
    {
        history_count++;
    }

    k_mutex_unlock(&shell_state_mutex);
}

/* Monitoring thread - updates system stats periodically */
static void shell_monitor_thread_main(void *p1, void *p2, void *p3)
{
    LOG_INF("Shell monitor thread started");

    while (1)
    {
        k_sleep(K_SECONDS(30));

        /* Periodic system health checks */
        struct system_stats stats;
        if (shell_get_system_stats(&stats) == 0)
        {
            LOG_DBG("System stats: uptime=%llums, heap=%zu/%zu bytes, threads=%u",
                    stats.uptime_ms, stats.heap_used, stats.heap_free, stats.thread_count);
        }
    }
}

/* Public API Implementation */
int akira_shell_init(void)
{
    int ret;

    /* Initialize GPIO pins */
    ret = init_gpio_pins();
    if (ret)
    {
        LOG_ERR("GPIO initialization failed: %d", ret);
        return ret;
    }

    /* Clear command history */
    memset(command_history, 0, sizeof(command_history));
    history_count = 0;
    history_index = 0;

    /* Clear aliases */
    memset(aliases, 0, sizeof(aliases));
    alias_count = 0;

    /* Start monitoring thread */
    shell_monitor_tid = k_thread_create(&shell_monitor_thread,
                                        shell_monitor_stack,
                                        K_THREAD_STACK_SIZEOF(shell_monitor_stack),
                                        shell_monitor_thread_main,
                                        NULL, NULL, NULL,
                                        SHELL_THREAD_PRIORITY,
                                        0, K_NO_WAIT);

    k_thread_name_set(shell_monitor_tid, "shell_monitor");

    LOG_INF("Akira shell module initialized");
    return 0;
}

int shell_get_system_stats(struct system_stats *stats)
{
    if (!stats)
    {
        return -EINVAL;
    }

    memset(stats, 0, sizeof(*stats));

    /* Get uptime */
    stats->uptime_ms = k_uptime_get();

    /* Heap statistics not available via sys_heap_runtime_stats_get for k_malloc.
       Set to 0 or use another supported API if you have a custom heap. */
    stats->heap_used = 0;
    stats->heap_free = 0;

    /* Count active threads */
    stats->thread_count = 0;
    // TODO: Implement thread counting if needed

    /* CPU usage - simplified estimation */
    stats->cpu_usage_percent = 25; // Placeholder

    /* Temperature - placeholder */
    stats->temperature_celsius = 45;

    /* WiFi stats - placeholder */
    stats->wifi_connected = true;
    stats->wifi_signal_strength = 75;

    return 0;
}

uint32_t shell_read_buttons(void)
{
    uint32_t button_state = 0;

    for (int i = 0; i < ARRAY_SIZE(button_specs); i++)
    {
        if (gpio_is_ready_dt(&button_specs[i]))
        {
            int val = gpio_pin_get_dt(&button_specs[i]);
            if (val == 0)
            { // Active low buttons
                button_state |= (1 << i);
            }
        }
    }

    return button_state;
}

int shell_control_display(const struct display_config *config)
{
    if (!config)
    {
        return -EINVAL;
    }

    k_mutex_lock(&shell_state_mutex, K_FOREVER);

    /* Update backlight */

    /* Store other settings (would need actual display driver integration) */
    current_display_config.brightness = config->brightness;
    current_display_config.rotation = config->rotation;
    current_display_config.inverted = config->inverted;

    k_mutex_unlock(&shell_state_mutex);

    LOG_INF("Display config updated: backlight=%s, brightness=%d, rotation=%d",
            config->backlight_on ? "on" : "off", config->brightness, config->rotation);

    return 0;
}

int shell_get_display_config(struct display_config *config)
{
    if (!config)
    {
        return -EINVAL;
    }

    k_mutex_lock(&shell_state_mutex, K_FOREVER);
    *config = current_display_config;
    k_mutex_unlock(&shell_state_mutex);

    return 0;
}

int shell_stress_test(uint32_t duration_seconds, uint8_t cpu_load)
{
    LOG_INF("Starting stress test: %us duration, %d%% CPU load", duration_seconds, cpu_load);

    uint64_t end_time = k_uptime_get() + (duration_seconds * 1000);
    uint32_t work_cycles = (cpu_load * 1000) / 100; // Work cycles per 1000

    while (k_uptime_get() < end_time)
    {
        /* Burn CPU cycles */
        for (uint32_t i = 0; i < work_cycles; i++)
        {
            __asm__ volatile("nop");
        }

        /* Sleep to achieve desired CPU load */
        k_sleep(K_USEC(1000 - work_cycles));

        /* Check if we should abort */
        if (k_uptime_get() % 5000 == 0)
        { // Every 5 seconds
            LOG_INF("Stress test in progress... %llu ms remaining",
                    (end_time - k_uptime_get()));
        }
    }

    LOG_INF("Stress test completed");
    return 0;
}

int shell_memory_dump(uintptr_t address, size_t length, char format)
{
    if (length > 4096)
    { // Safety limit
        return -EINVAL;
    }

    const uint8_t *data = (const uint8_t *)address;

    LOG_INF("Memory dump from 0x%08lx (%zu bytes):", address, length);

    for (size_t i = 0; i < length; i += 16)
    {
        /* Print address */
        printk("%08lx: ", address + i);

        /* Print hex bytes */
        for (size_t j = 0; j < 16 && (i + j) < length; j++)
        {
            printk("%02x ", data[i + j]);
        }

        /* Pad if needed */
        for (size_t j = length - i; j < 16; j++)
        {
            printk("   ");
        }

        /* Print ASCII representation if requested */
        if (format == 'a' || format == 'm')
        {
            printk(" |");
            for (size_t j = 0; j < 16 && (i + j) < length; j++)
            {
                char c = data[i + j];
                printk("%c", (c >= 32 && c <= 126) ? c : '.');
            }
            printk("|");
        }

        printk("\n");
    }

    return 0;
}

int shell_add_alias(const char *alias, const char *command)
{
    if (!alias || !command || alias_count >= MAX_ALIAS_COUNT)
    {
        return -EINVAL;
    }

    k_mutex_lock(&shell_state_mutex, K_FOREVER);

    strncpy(aliases[alias_count].alias, alias, sizeof(aliases[0].alias) - 1);
    strncpy(aliases[alias_count].command, command, sizeof(aliases[0].command) - 1);
    aliases[alias_count].active = true;
    alias_count++;

    k_mutex_unlock(&shell_state_mutex);

    LOG_INF("Added alias: '%s' -> '%s'", alias, command);
    return 0;
}

int shell_get_command_history(char history[][64], size_t max_entries)
{
    if (!history)
    {
        return -EINVAL;
    }

    k_mutex_lock(&shell_state_mutex, K_FOREVER);

    size_t entries_to_copy = (history_count < max_entries) ? history_count : max_entries;

    for (size_t i = 0; i < entries_to_copy; i++)
    {
        size_t src_index = (history_index - entries_to_copy + i + MAX_COMMAND_HISTORY) % MAX_COMMAND_HISTORY;
        strncpy(history[i], command_history[src_index], 63);
        history[i][63] = '\0';
    }

    k_mutex_unlock(&shell_state_mutex);

    return entries_to_copy;
}

void shell_clear_history(void)
{
    k_mutex_lock(&shell_state_mutex, K_FOREVER);
    memset(command_history, 0, sizeof(command_history));
    history_count = 0;
    history_index = 0;
    k_mutex_unlock(&shell_state_mutex);
}

/* Akira Shell Commands */

static int cmd_system_info(const struct shell *sh, size_t argc, char **argv)
{
    struct system_stats stats;

    if (shell_get_system_stats(&stats) != 0)
    {
        shell_error(sh, "Failed to get system statistics");
        return -EIO;
    }

    shell_print(sh, "\n=== ESP32 Gaming System Information ===");
    shell_print(sh, "Uptime: %llu ms (%llu seconds)", stats.uptime_ms, stats.uptime_ms / 1000);
    shell_print(sh, "Memory: %zu bytes used, %zu bytes free", stats.heap_used, stats.heap_free);
    shell_print(sh, "Active threads: %u", stats.thread_count);
    shell_print(sh, "CPU usage: %u%%", stats.cpu_usage_percent);
    shell_print(sh, "Temperature: %d°C", stats.temperature_celsius);
    shell_print(sh, "WiFi: %s (Signal: %u%%)",
                stats.wifi_connected ? "Connected" : "Disconnected",
                stats.wifi_signal_strength);

    return 0;
}

static int cmd_buttons_read(const struct shell *sh, size_t argc, char **argv)
{
    uint32_t button_state = shell_read_buttons();

    shell_print(sh, "\n=== Gaming Button States ===");
    shell_print(sh, "ON/OFF:   %s", (button_state & BTN_ONOFF) ? "PRESSED" : "Released");
    shell_print(sh, "Settings: %s", (button_state & BTN_SETTINGS) ? "PRESSED" : "Released");
    shell_print(sh, "D-Pad Up: %s", (button_state & BTN_UP) ? "PRESSED" : "Released");
    shell_print(sh, "D-Pad Down: %s", (button_state & BTN_DOWN) ? "PRESSED" : "Released");
    shell_print(sh, "D-Pad Left: %s", (button_state & BTN_LEFT) ? "PRESSED" : "Released");
    shell_print(sh, "D-Pad Right: %s", (button_state & BTN_RIGHT) ? "PRESSED" : "Released");
    shell_print(sh, "Button A: %s", (button_state & BTN_A) ? "PRESSED" : "Released");
    shell_print(sh, "Button B: %s", (button_state & BTN_B) ? "PRESSED" : "Released");
    shell_print(sh, "Button X: %s", (button_state & BTN_X) ? "PRESSED" : "Released");
    shell_print(sh, "Button Y: %s", (button_state & BTN_Y) ? "PRESSED" : "Released");

    if (button_state == 0)
    {
        shell_print(sh, "No buttons are currently pressed");
    }

    return 0;
}

static int cmd_display_control(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2)
    {
        struct display_config config;
        shell_get_display_config(&config);

        shell_print(sh, "\n=== Display Configuration ===");
        shell_print(sh, "Backlight: %s", config.backlight_on ? "ON" : "OFF");
        shell_print(sh, "Brightness: %d/255", config.brightness);
        shell_print(sh, "Rotation: %d degrees", config.rotation);
        shell_print(sh, "Inverted: %s", config.inverted ? "YES" : "NO");
        shell_print(sh, "\nUsage: display <backlight|brightness|rotation> <value>");
        return 0;
    }

    struct display_config config;
    shell_get_display_config(&config);

    if (strcmp(argv[1], "backlight") == 0)
    {
        if (argc < 3)
        {
            shell_error(sh, "Usage: display backlight <on|off>");
            return -EINVAL;
        }
        config.backlight_on = (strcmp(argv[2], "on") == 0);
        shell_control_display(&config);
        shell_print(sh, "Display backlight %s", config.backlight_on ? "enabled" : "disabled");
    }
    else if (strcmp(argv[1], "brightness") == 0)
    {
        if (argc < 3)
        {
            shell_error(sh, "Usage: display brightness <0-255>");
            return -EINVAL;
        }
        int brightness = atoi(argv[2]);
        if (brightness < 0 || brightness > 255)
        {
            shell_error(sh, "Brightness must be 0-255");
            return -EINVAL;
        }
        config.brightness = brightness;
        shell_control_display(&config);
        shell_print(sh, "Display brightness set to %d", brightness);
    }
    else if (strcmp(argv[1], "rotation") == 0)
    {
        if (argc < 3)
        {
            shell_error(sh, "Usage: display rotation <0|90|180|270>");
            return -EINVAL;
        }
        int rotation = atoi(argv[2]);
        if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)
        {
            shell_error(sh, "Rotation must be 0, 90, 180, or 270 degrees");
            return -EINVAL;
        }
        config.rotation = rotation;
        shell_control_display(&config);
        shell_print(sh, "Display rotation set to %d degrees", rotation);
    }
    else
    {
        shell_error(sh, "Unknown display parameter: %s", argv[1]);
        return -EINVAL;
    }

    return 0;
}

static int cmd_stress_test(const struct shell *sh, size_t argc, char **argv)
{
    uint32_t duration = 10;
    uint8_t cpu_load = 50;

    if (argc > 1)
    {
        duration = strtoul(argv[1], NULL, 10);
        if (duration > 300)
        { // Max 5 minutes
            shell_error(sh, "Maximum duration is 300 seconds");
            return -EINVAL;
        }
    }

    if (argc > 2)
    {
        cpu_load = strtoul(argv[2], NULL, 10);
        if (cpu_load > 100)
        {
            shell_error(sh, "CPU load must be 1-100");
            return -EINVAL;
        }
    }

    shell_print(sh, "Starting stress test: %us duration, %d%% CPU load", duration, cpu_load);
    shell_print(sh, "Press Ctrl+C to abort");

    return shell_stress_test(duration, cpu_load);
}

static int cmd_memory_dump(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 3)
    {
        shell_error(sh, "Usage: memdump <address> <length> [format]");
        shell_print(sh, "Format: h=hex, a=ascii, m=mixed (default: h)");
        return -EINVAL;
    }

    uintptr_t address = strtoul(argv[1], NULL, 0);
    size_t length = strtoul(argv[2], NULL, 10);
    char format = 'h';

    if (argc > 3)
    {
        format = argv[3][0];
        if (format != 'h' && format != 'a' && format != 'm')
        {
            shell_error(sh, "Invalid format. Use h, a, or m");
            return -EINVAL;
        }
    }

    return shell_memory_dump(address, length, format);
}

static int cmd_alias(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_print(sh, "\n=== Command Aliases ===");
        k_mutex_lock(&shell_state_mutex, K_FOREVER);
        for (size_t i = 0; i < alias_count; i++)
        {
            if (aliases[i].active)
            {
                shell_print(sh, "%s -> %s", aliases[i].alias, aliases[i].command);
            }
        }
        k_mutex_unlock(&shell_state_mutex);
        shell_print(sh, "\nUsage: alias <name> <command>");
        return 0;
    }

    if (argc < 3)
    {
        shell_error(sh, "Usage: alias <name> <command>");
        return -EINVAL;
    }

    int ret = shell_add_alias(argv[1], argv[2]);
    if (ret == 0)
    {
        shell_print(sh, "Alias '%s' created for '%s'", argv[1], argv[2]);
    }
    else
    {
        shell_error(sh, "Failed to create alias: %d", ret);
    }

    return ret;
}

static int cmd_history(const struct shell *sh, size_t argc, char **argv)
{
    char history[MAX_COMMAND_HISTORY][64];
    int count = shell_get_command_history(history, MAX_COMMAND_HISTORY);

    shell_print(sh, "\n=== Command History ===");
    for (int i = 0; i < count; i++)
    {
        shell_print(sh, "%2d: %s", i + 1, history[i]);
    }

    if (count == 0)
    {
        shell_print(sh, "No commands in history");
    }

    return 0;
}

static int cmd_clear_history(const struct shell *sh, size_t argc, char **argv)
{
    shell_clear_history();
    shell_print(sh, "Command history cleared");
    return 0;
}

static int cmd_threads_info(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "\n=== Thread Information ===");
    shell_print(sh, "%-16s %-8s %-8s %-10s %-10s", "Name", "State", "Priority", "Stack Used", "Stack Size");
    shell_print(sh, "----------------------------------------------------------------");

    // This is a simplified version - would need actual thread enumeration
    shell_print(sh, "%-16s %-8s %-8d %-10s %-10s", "main", "running", 0, "2048", "4096");
    shell_print(sh, "%-16s %-8s %-8d %-10s %-10s", "shell_monitor", "sleeping", SHELL_THREAD_PRIORITY, "512", "2048");
    shell_print(sh, "%-16s %-8s %-8d %-10s %-10s", "ota_manager", "sleeping", 6, "1024", "4096");
    shell_print(sh, "%-16s %-8s %-8d %-10s %-10s", "settings", "sleeping", 7, "768", "2048");

    return 0;
}

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
    uint32_t delay = 3;

    if (argc > 1)
    {
        delay = strtoul(argv[1], NULL, 10);
        if (delay > 60)
        {
            delay = 60;
        }
    }

    shell_print(sh, "System will reboot in %u seconds...", delay);
    shell_print(sh, "Press Ctrl+C to abort");

    for (uint32_t i = delay; i > 0; i--)
    {
        shell_print(sh, "Rebooting in %u...", i);
        k_sleep(K_SECONDS(1));
    }

    LOG_INF("System reboot requested via shell");
    sys_reboot(SYS_REBOOT_WARM);

    return 0;
}

/* Shell command registration */
SHELL_STATIC_SUBCMD_SET_CREATE(system_cmds,
                               SHELL_CMD(info, NULL, "Show comprehensive system information", cmd_system_info),
                               SHELL_CMD(stress, NULL, "Run CPU stress test [duration] [cpu_load%]", cmd_stress_test),
                               SHELL_CMD(threads, NULL, "Show thread information", cmd_threads_info),
                               SHELL_CMD(reboot, NULL, "Reboot system [delay_seconds]", cmd_reboot),
                               SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(gaming_cmds,
                               SHELL_CMD(buttons, NULL, "Read gaming button states", cmd_buttons_read),
                               SHELL_CMD(display, NULL, "Control display settings", cmd_display_control),
                               SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(debug_cmds,
                               SHELL_CMD(memdump, NULL, "Dump memory contents", cmd_memory_dump),
                               SHELL_CMD(alias, NULL, "Create command alias", cmd_alias),
                               SHELL_CMD(history, NULL, "Show command history", cmd_history),
                               SHELL_CMD(clear_history, NULL, "Clear command history", cmd_clear_history),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(sys, &system_cmds, "System management commands", NULL);
SHELL_CMD_REGISTER(game, &gaming_cmds, "Gaming-specific commands", NULL);
SHELL_CMD_REGISTER(debug, &debug_cmds, "Debug and diagnostic commands", NULL);