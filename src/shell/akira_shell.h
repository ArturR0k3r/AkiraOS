/**
 * @file akira_shell.h
 * @brief Akira Shell Module for ESP32 Gaming Device
 *
 * Provides comprehensive system management commands including
 * hardware control, diagnostics, and gaming-specific features.
 */

#ifndef AKIRA_SHELL_H
#define AKIRA_SHELL_H

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <stdint.h>
#include <stdbool.h>

/* Shell module configuration */
#define SHELL_THREAD_STACK_SIZE 2048
#define SHELL_THREAD_PRIORITY 8
#define MAX_COMMAND_HISTORY 10
#define MAX_ALIAS_COUNT 20

/* Gaming button definitions */
enum gaming_button
{
    BTN_NONE = 0,
    BTN_ONOFF = (1 << 0),
    BTN_SETTINGS = (1 << 1),
    BTN_UP = (1 << 2),
    BTN_DOWN = (1 << 3),
    BTN_LEFT = (1 << 4),
    BTN_RIGHT = (1 << 5),
    BTN_A = (1 << 6),
    BTN_B = (1 << 7),
    BTN_X = (1 << 8),
    BTN_Y = (1 << 9)
};

/* System statistics */
struct system_stats
{
    uint64_t uptime_ms;
    size_t heap_used;
    size_t heap_free;
    uint32_t thread_count;
    uint32_t cpu_usage_percent;
    int32_t temperature_celsius;
    uint32_t wifi_signal_strength;
    bool wifi_connected;
};

/* Display control */
struct display_config
{
    bool backlight_on;
    uint8_t brightness; // 0-255
    uint16_t rotation;  // 0, 90, 180, 270
    bool inverted;
};

/**
 * @brief Initialize the Akira shell module
 *
 * Sets up hardware interfaces, command history, and starts monitoring thread
 *
 * @return 0 on success, negative error code on failure
 */
int akira_shell_init(void);

/**
 * @brief Get current system statistics
 *
 * @param stats Output buffer for system statistics
 * @return 0 on success, negative error code on failure
 */
int shell_get_system_stats(struct system_stats *stats);

/**
 * @brief Read gaming button states
 *
 * @return Bitmask of pressed buttons (enum gaming_button)
 */
uint32_t shell_read_buttons(void);

/**
 * @brief Control display settings
 *
 * @param config Display configuration to apply
 * @return 0 on success, negative error code on failure
 */
int shell_control_display(const struct display_config *config);

/**
 * @brief Get current display configuration
 *
 * @param config Output buffer for current display config
 * @return 0 on success, negative error code on failure
 */
int shell_get_display_config(struct display_config *config);

/**
 * @brief Perform system stress test
 *
 * @param duration_seconds Test duration in seconds
 * @param cpu_load Target CPU load percentage (1-100)
 * @return 0 on success, negative error code on failure
 */
int shell_stress_test(uint32_t duration_seconds, uint8_t cpu_load);

/**
 * @brief Memory dump utility
 *
 * @param address Starting memory address
 * @param length Number of bytes to dump
 * @param format Output format ('h'=hex, 'a'=ascii, 'm'=mixed)
 * @return 0 on success, negative error code on failure
 */
int shell_memory_dump(uintptr_t address, size_t length, char format);

/**
 * @brief Execute system command with timeout
 *
 * @param command Command string to execute
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, negative error code on failure
 */
int shell_execute_with_timeout(const char *command, uint32_t timeout_ms);

/**
 * @brief Register custom shell command
 *
 * @param name Command name
 * @param help Help text
 * @param handler Command handler function
 * @return 0 on success, negative error code on failure
 */
int shell_register_custom_command(const char *name, const char *help,
                                  shell_cmd_handler handler);

/**
 * @brief Add command alias
 *
 * @param alias Alias name
 * @param command Target command
 * @return 0 on success, negative error code on failure
 */
int shell_add_alias(const char *alias, const char *command);

/**
 * @brief Get command history
 *
 * @param history Output array for history entries
 * @param max_entries Maximum number of entries to return
 * @return Number of history entries returned
 */
int shell_get_command_history(char history[][64], size_t max_entries);

/**
 * @brief Clear command history
 */
void shell_clear_history(void);

/**
 * @brief Save shell configuration to persistent storage
 *
 * @return 0 on success, negative error code on failure
 */
int shell_save_config(void);

/**
 * @brief Load shell configuration from persistent storage
 *
 * @return 0 on success, negative error code on failure
 */
int shell_load_config(void);

#endif /* AKIRA_SHELL_H */