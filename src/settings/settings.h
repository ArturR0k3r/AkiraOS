/**
 * @file settings.h
 * @brief User settings management module for Akira Board
 *
 * This module provides persistent configuration storage and management
 * running on a dedicated thread for non-blocking operations.
 */

#ifndef USER_SETTINGS_H
#define USER_SETTINGS_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

/* Configuration constants */
#define MAX_DEVICE_ID_LEN 32
#define MAX_WIFI_SSID_LEN 32
#define MAX_WIFI_PASSCODE_LEN 64
#define SETTINGS_THREAD_STACK_SIZE 2048
#define SETTINGS_THREAD_PRIORITY 7

/* Settings keys */
#define DEVICE_ID_KEY "device/id"
#define WIFI_SSID_KEY "wifi/ssid"
#define WIFI_PASSCODE_KEY "wifi/passcode"
#define WIFI_ENABLED_KEY "wifi/enabled"

/**
 * @brief User settings structure
 */
struct user_settings
{
    char device_id[MAX_DEVICE_ID_LEN];
    char wifi_ssid[MAX_WIFI_SSID_LEN];
    char wifi_passcode[MAX_WIFI_PASSCODE_LEN];
    bool wifi_enabled;
    /* Add more settings here as needed */
};

/**
 * @brief Settings operation result codes
 */
enum settings_result
{
    SETTINGS_OK = 0,
    SETTINGS_ERROR = -1,
    SETTINGS_NOT_FOUND = -2,
    SETTINGS_INVALID_PARAM = -3,
    SETTINGS_BUFFER_TOO_SMALL = -4,
    SETTINGS_SAVE_FAILED = -5
};

/**
 * @brief Settings change callback function type
 *
 * @param key Setting key that changed
 * @param value New value (can be NULL for deletions)
 * @param user_data User-provided callback data
 */
typedef void (*settings_change_cb_t)(const char *key, const void *value, void *user_data);

/**
 * @brief Initialize the settings module
 *
 * Starts the settings thread and initializes the storage subsystem.
 *
 * @return 0 on success, negative error code on failure
 */
int user_settings_init(void);

/**
 * @brief Get current settings structure
 *
 * @return Pointer to current settings (read-only)
 */
const struct user_settings *user_settings_get(void);

/**
 * @brief Set device ID
 *
 * @param device_id Device ID string (max MAX_DEVICE_ID_LEN-1 characters)
 * @return settings_result code
 */
enum settings_result user_settings_set_device_id(const char *device_id);

/**
 * @brief Set WiFi credentials
 *
 * @param ssid WiFi SSID (max MAX_WIFI_SSID_LEN-1 characters)
 * @param passcode WiFi passcode (max MAX_WIFI_PASSCODE_LEN-1 characters)
 * @return settings_result code
 */
enum settings_result user_settings_set_wifi_credentials(const char *ssid, const char *passcode);

/**
 * @brief Enable/disable WiFi
 *
 * @param enabled WiFi enabled state
 * @return settings_result code
 */
enum settings_result user_settings_set_wifi_enabled(bool enabled);

/**
 * @brief Save all settings to persistent storage
 *
 * @return settings_result code
 */
enum settings_result user_settings_save(void);

/**
 * @brief Load settings from persistent storage
 *
 * @return settings_result code
 */
enum settings_result user_settings_load(void);

/**
 * @brief Reset all settings to defaults
 *
 * @return settings_result code
 */
enum settings_result user_settings_reset(void);

/**
 * @brief Register callback for settings changes
 *
 * @param callback Callback function to call when settings change
 * @param user_data User data to pass to callback
 * @return settings_result code
 */
enum settings_result user_settings_register_callback(settings_change_cb_t callback, void *user_data);

/**
 * @brief Print current settings (for debugging)
 */
void user_settings_print(void);

/**
 * @brief Get settings as JSON string
 *
 * @param buffer Output buffer for JSON string
 * @param buffer_size Size of output buffer
 * @return Length of JSON string on success, negative error code on failure
 */
int user_settings_to_json(char *buffer, size_t buffer_size);

#endif /* USER_SETTINGS_H */