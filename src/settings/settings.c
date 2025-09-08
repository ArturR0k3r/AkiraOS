/**
 * @file settings.c
 * @brief Optimized user settings management module implementation
 *
 * Provides thread-safe persistent settings storage with change callbacks
 * and shell command integration for the Akira Board OTA system.
 * Runs on a dedicated thread for non-blocking operations.
 * Uses atomic operations and optimized data structures.
 * Minimizes RAM usage with const defaults and efficient key handling.
 */

#include "settings.h"
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(user_settings, LOG_LEVEL_INF);

/* Optimized settings storage with atomic operations */
static struct user_settings current_settings;
static K_MUTEX_DEFINE(settings_mutex);
static atomic_t settings_dirty = ATOMIC_INIT(0);

/* Default settings - const to save RAM */
static const struct user_settings default_settings = {
    .device_id = "akira-default",
    .wifi_ssid = "",
    .wifi_passcode = "",
    .wifi_enabled = false};

/* Optimized callback system */
struct callback_node
{
    settings_change_cb_t callback;
    void *user_data;
    struct callback_node *next;
};

static struct callback_node *callback_list = NULL;
static K_MUTEX_DEFINE(callback_mutex);

/* Work queue for asynchronous operations */
static struct k_work_q settings_workq;
static K_THREAD_STACK_DEFINE(settings_workq_stack, 4096);

/* Work items for different operations */
static struct k_work save_work;
static struct k_work load_work;

/* Settings key definitions - optimized as macros */

/* Validation helpers */
static inline bool is_valid_device_id(const char *id)
{
    return id && strlen(id) > 0 && strlen(id) < MAX_DEVICE_ID_LEN;
}

static inline bool is_valid_wifi_ssid(const char *ssid)
{
    return ssid && strlen(ssid) < MAX_WIFI_SSID_LEN;
}

static inline bool is_valid_wifi_passcode(const char *passcode)
{
    return passcode && strlen(passcode) < MAX_WIFI_PASSCODE_LEN;
}

/* Settings handlers for Zephyr settings subsystem */
static int settings_set_handler(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
    int ret;
    const char *subkey = key;

    /* Parse key hierarchy */
    if (strncmp(subkey, "device_id", 9) == 0)
    {
        ret = read_cb(cb_arg, current_settings.device_id, sizeof(current_settings.device_id));
        if (ret > 0)
        {
            current_settings.device_id[MIN(ret, MAX_DEVICE_ID_LEN - 1)] = '\0';
            LOG_DBG("Loaded device ID");
        }
    }
    else if (strncmp(subkey, "wifi_ssid", 9) == 0)
    {
        ret = read_cb(cb_arg, current_settings.wifi_ssid, sizeof(current_settings.wifi_ssid));
        if (ret > 0)
        {
            current_settings.wifi_ssid[MIN(ret, MAX_WIFI_SSID_LEN - 1)] = '\0';
            LOG_DBG("Loaded WiFi SSID");
        }
    }
    else if (strncmp(subkey, "wifi_passcode", 13) == 0)
    {
        ret = read_cb(cb_arg, current_settings.wifi_passcode, sizeof(current_settings.wifi_passcode));
        if (ret > 0)
        {
            current_settings.wifi_passcode[MIN(ret, MAX_WIFI_PASSCODE_LEN - 1)] = '\0';
            LOG_DBG("Loaded WiFi passcode");
        }
    }
    else if (strncmp(subkey, "wifi_enabled", 12) == 0)
    {
        uint8_t enabled;
        ret = read_cb(cb_arg, &enabled, sizeof(enabled));
        if (ret > 0)
        {
            current_settings.wifi_enabled = (enabled != 0);
            LOG_DBG("Loaded WiFi enabled: %s", enabled ? "true" : "false");
        }
    }

    return 0;
}

static int settings_commit_handler(void)
{
    LOG_INF("Settings loaded from persistent storage");
    atomic_clear(&settings_dirty);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(user_settings, "user", NULL, settings_set_handler, settings_commit_handler, NULL);

/* Optimized callback notification */
static void notify_callbacks(const char *key, const void *value)
{
    if (k_mutex_lock(&callback_mutex, K_MSEC(100)) != 0)
    {
        LOG_WRN("Failed to acquire callback mutex");
        return;
    }

    struct callback_node *node = callback_list;
    while (node)
    {
        if (node->callback)
        {
            node->callback(key, value, node->user_data);
        }
        node = node->next;
    }

    k_mutex_unlock(&callback_mutex);
}

/* Optimized save operation */
static enum settings_result save_setting_atomic(const char *key, const void *value, size_t len)
{
    int ret = settings_save_one(key, value, len);
    if (ret != 0)
    {
        LOG_ERR("Failed to save %s: %d", key, ret);
        return SETTINGS_SAVE_FAILED;
    }
    return SETTINGS_OK;
}

/* Work handlers */
static void save_work_handler(struct k_work *work)
{
    enum settings_result result = SETTINGS_OK;

    if (!atomic_test_and_clear_bit(&settings_dirty, 0))
    {
        return; /* Nothing to save */
    }

    k_mutex_lock(&settings_mutex, K_FOREVER);

    /* Save all settings atomically */
    if (save_setting_atomic(DEVICE_ID_KEY, current_settings.device_id,
                            strlen(current_settings.device_id)) != SETTINGS_OK ||
        save_setting_atomic(WIFI_SSID_KEY, current_settings.wifi_ssid,
                            strlen(current_settings.wifi_ssid)) != SETTINGS_OK ||
        save_setting_atomic(WIFI_PASSCODE_KEY, current_settings.wifi_passcode,
                            strlen(current_settings.wifi_passcode)) != SETTINGS_OK)
    {
        result = SETTINGS_SAVE_FAILED;
    }

    uint8_t enabled = current_settings.wifi_enabled ? 1 : 0;
    if (save_setting_atomic(WIFI_ENABLED_KEY, &enabled, sizeof(enabled)) != SETTINGS_OK)
    {
        result = SETTINGS_SAVE_FAILED;
    }

    k_mutex_unlock(&settings_mutex);

    if (result == SETTINGS_OK)
    {
        LOG_INF("Settings saved successfully");
    }
    else
    {
        LOG_ERR("Failed to save settings");
        atomic_set_bit(&settings_dirty, 0); /* Retry later */
    }
}

static void load_work_handler(struct k_work *work)
{
    int ret = settings_load();
    if (ret != 0)
    {
        LOG_ERR("Failed to load settings: %d", ret);
        return;
    }
    LOG_INF("Settings loaded successfully");
}

/* Public API implementation */
int user_settings_init(void)
{
    /* Initialize current settings with defaults */
    memcpy(&current_settings, &default_settings, sizeof(current_settings));

    /* Initialize settings subsystem */
    int ret = settings_subsys_init();
    if (ret)
    {
        LOG_ERR("Settings subsystem init failed: %d", ret);
        return ret;
    }

    /* Initialize work queue */
    k_work_queue_init(&settings_workq);
    k_work_queue_start(&settings_workq, settings_workq_stack,
                       K_THREAD_STACK_SIZEOF(settings_workq_stack),
                       K_PRIO_COOP(10), NULL);

    /* Initialize work items */
    k_work_init(&save_work, save_work_handler);
    k_work_init(&load_work, load_work_handler);

    /* Load existing settings */
    k_work_submit_to_queue(&settings_workq, &load_work);

    LOG_INF("User settings module initialized");
    return 0;
}

const struct user_settings *user_settings_get(void)
{
    return &current_settings;
}

enum settings_result user_settings_get_copy(struct user_settings *settings_copy)
{
    if (!settings_copy)
    {
        return SETTINGS_INVALID_PARAM;
    }

    k_mutex_lock(&settings_mutex, K_FOREVER);
    memcpy(settings_copy, &current_settings, sizeof(*settings_copy));
    k_mutex_unlock(&settings_mutex);

    return SETTINGS_OK;
}

enum settings_result user_settings_set_device_id(const char *device_id)
{
    if (!is_valid_device_id(device_id))
    {
        return SETTINGS_INVALID_PARAM;
    }

    k_mutex_lock(&settings_mutex, K_FOREVER);

    if (strcmp(current_settings.device_id, device_id) == 0)
    {
        k_mutex_unlock(&settings_mutex);
        return SETTINGS_OK; /* No change needed */
    }

    strncpy(current_settings.device_id, device_id, MAX_DEVICE_ID_LEN - 1);
    current_settings.device_id[MAX_DEVICE_ID_LEN - 1] = '\0';
    atomic_set_bit(&settings_dirty, 0);

    k_mutex_unlock(&settings_mutex);

    LOG_INF("Device ID updated: %s", device_id);
    notify_callbacks(DEVICE_ID_KEY, device_id);

    return SETTINGS_OK;
}

enum settings_result user_settings_set_wifi_credentials(const char *ssid, const char *passcode)
{
    if (!is_valid_wifi_ssid(ssid) || !is_valid_wifi_passcode(passcode))
    {
        return SETTINGS_INVALID_PARAM;
    }

    k_mutex_lock(&settings_mutex, K_FOREVER);

    bool changed = (strcmp(current_settings.wifi_ssid, ssid) != 0) ||
                   (strcmp(current_settings.wifi_passcode, passcode) != 0);

    if (!changed)
    {
        k_mutex_unlock(&settings_mutex);
        return SETTINGS_OK; /* No change needed */
    }

    strncpy(current_settings.wifi_ssid, ssid, MAX_WIFI_SSID_LEN - 1);
    current_settings.wifi_ssid[MAX_WIFI_SSID_LEN - 1] = '\0';
    strncpy(current_settings.wifi_passcode, passcode, MAX_WIFI_PASSCODE_LEN - 1);
    current_settings.wifi_passcode[MAX_WIFI_PASSCODE_LEN - 1] = '\0';
    atomic_set_bit(&settings_dirty, 0);

    k_mutex_unlock(&settings_mutex);

    LOG_INF("WiFi credentials updated: %s", ssid);
    notify_callbacks(WIFI_SSID_KEY, ssid);
    notify_callbacks(WIFI_PASSCODE_KEY, passcode);

    return SETTINGS_OK;
}

enum settings_result user_settings_set_wifi_enabled(bool enabled)
{
    k_mutex_lock(&settings_mutex, K_FOREVER);

    if (current_settings.wifi_enabled == enabled)
    {
        k_mutex_unlock(&settings_mutex);
        return SETTINGS_OK; /* No change needed */
    }

    current_settings.wifi_enabled = enabled;
    atomic_set_bit(&settings_dirty, 0);

    k_mutex_unlock(&settings_mutex);

    LOG_INF("WiFi %s", enabled ? "enabled" : "disabled");
    notify_callbacks(WIFI_ENABLED_KEY, &enabled);

    return SETTINGS_OK;
}

enum settings_result user_settings_save(void)
{
    if (!atomic_test_bit(&settings_dirty, 0))
    {
        return SETTINGS_OK; /* Nothing to save */
    }

    return k_work_submit_to_queue(&settings_workq, &save_work) ? SETTINGS_OK : SETTINGS_ERROR;
}

enum settings_result user_settings_save_sync(void)
{
    save_work_handler(&save_work);
    return atomic_test_bit(&settings_dirty, 0) ? SETTINGS_SAVE_FAILED : SETTINGS_OK;
}

enum settings_result user_settings_load(void)
{
    return k_work_submit_to_queue(&settings_workq, &load_work) ? SETTINGS_OK : SETTINGS_ERROR;
}

enum settings_result user_settings_reset(void)
{
    k_mutex_lock(&settings_mutex, K_FOREVER);
    memcpy(&current_settings, &default_settings, sizeof(current_settings));
    atomic_set_bit(&settings_dirty, 0);
    k_mutex_unlock(&settings_mutex);

    LOG_INF("Settings reset to defaults");
    notify_callbacks("*", NULL); /* Notify all settings changed */

    return user_settings_save_sync();
}

enum settings_result user_settings_register_callback(settings_change_cb_t callback, void *user_data)
{
    if (!callback)
    {
        return SETTINGS_INVALID_PARAM;
    }

    struct callback_node *new_node = k_malloc(sizeof(struct callback_node));
    if (!new_node)
    {
        return SETTINGS_ERROR;
    }

    new_node->callback = callback;
    new_node->user_data = user_data;

    k_mutex_lock(&callback_mutex, K_FOREVER);
    new_node->next = callback_list;
    callback_list = new_node;
    k_mutex_unlock(&callback_mutex);

    return SETTINGS_OK;
}

enum settings_result user_settings_unregister_callback(settings_change_cb_t callback)
{
    if (!callback)
    {
        return SETTINGS_INVALID_PARAM;
    }

    k_mutex_lock(&callback_mutex, K_FOREVER);

    struct callback_node **current = &callback_list;
    while (*current)
    {
        if ((*current)->callback == callback)
        {
            struct callback_node *to_remove = *current;
            *current = (*current)->next;
            k_free(to_remove);
            k_mutex_unlock(&callback_mutex);
            return SETTINGS_OK;
        }
        current = &(*current)->next;
    }

    k_mutex_unlock(&callback_mutex);
    return SETTINGS_ERROR;
}

void user_settings_print(void)
{
    struct user_settings settings_copy;
    if (user_settings_get_copy(&settings_copy) != SETTINGS_OK)
    {
        printk("Failed to get settings\n");
        return;
    }

    printk("\n=== User Settings ===\n");
    printk("Device ID: %s\n", settings_copy.device_id);
    printk("WiFi SSID: %s\n", settings_copy.wifi_ssid);
    printk("WiFi Enabled: %s\n", settings_copy.wifi_enabled ? "Yes" : "No");
    printk("WiFi Passcode: %s\n",
           strlen(settings_copy.wifi_passcode) > 0 ? "***SET***" : "***NOT SET***");
}

int user_settings_to_json(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size < 100)
    {
        return SETTINGS_BUFFER_TOO_SMALL;
    }

    struct user_settings settings_copy;
    if (user_settings_get_copy(&settings_copy) != SETTINGS_OK)
    {
        return SETTINGS_ERROR;
    }

    /* Use more efficient formatting */
    int len = snprintf(buffer, buffer_size,
                       "{\"device_id\":\"%s\",\"wifi_ssid\":\"%s\",\"wifi_enabled\":%s,\"wifi_passcode_set\":%s}",
                       settings_copy.device_id,
                       settings_copy.wifi_ssid,
                       settings_copy.wifi_enabled ? "true" : "false",
                       strlen(settings_copy.wifi_passcode) > 0 ? "true" : "false");

    return (len < buffer_size) ? len : SETTINGS_BUFFER_TOO_SMALL;
}

/* Optimized shell commands with better error handling */
static int cmd_settings_show(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    struct user_settings settings_copy;
    if (user_settings_get_copy(&settings_copy) != SETTINGS_OK)
    {
        shell_error(sh, "Failed to get settings");
        return -EIO;
    }

    shell_print(sh, "\n=== Current Settings ===");
    shell_print(sh, "Device ID: %s", settings_copy.device_id);
    shell_print(sh, "WiFi SSID: %s", settings_copy.wifi_ssid);
    shell_print(sh, "WiFi Enabled: %s", settings_copy.wifi_enabled ? "Yes" : "No");
    shell_print(sh, "WiFi Passcode: %s",
                strlen(settings_copy.wifi_passcode) > 0 ? "***SET***" : "***NOT SET***");

    return 0;
}

static int cmd_settings_set_device_id(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_error(sh, "Usage: settings set_device_id <device_id>");
        return -EINVAL;
    }

    enum settings_result result = user_settings_set_device_id(argv[1]);
    switch (result)
    {
    case SETTINGS_OK:
        shell_print(sh, "Device ID set to: %s", argv[1]);
        break;
    case SETTINGS_INVALID_PARAM:
        shell_error(sh, "Invalid device ID (too long or empty)");
        break;
    default:
        shell_error(sh, "Failed to set device ID: %d", result);
        break;
    }

    return 0;
}

static int cmd_settings_set_wifi(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 3)
    {
        shell_error(sh, "Usage: settings set_wifi <ssid> <passcode>");
        return -EINVAL;
    }

    enum settings_result result = user_settings_set_wifi_credentials(argv[1], argv[2]);
    switch (result)
    {
    case SETTINGS_OK:
        shell_print(sh, "WiFi credentials updated for SSID: %s", argv[1]);
        break;
    case SETTINGS_INVALID_PARAM:
        shell_error(sh, "Invalid WiFi credentials (too long)");
        break;
    default:
        shell_error(sh, "Failed to set WiFi credentials: %d", result);
        break;
    }

    return 0;
}

static int cmd_settings_wifi_enable(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_error(sh, "Usage: settings wifi_enable <true|false>");
        return -EINVAL;
    }

    bool enabled = (strcmp(argv[1], "true") == 0) || (strcmp(argv[1], "1") == 0);

    enum settings_result result = user_settings_set_wifi_enabled(enabled);
    if (result == SETTINGS_OK)
    {
        shell_print(sh, "WiFi %s", enabled ? "enabled" : "disabled");
    }
    else
    {
        shell_error(sh, "Failed to set WiFi state: %d", result);
    }

    return 0;
}

static int cmd_settings_save(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    enum settings_result result = user_settings_save_sync();
    if (result == SETTINGS_OK)
    {
        shell_print(sh, "Settings saved successfully");
    }
    else
    {
        shell_error(sh, "Failed to save settings: %d", result);
    }

    return 0;
}

static int cmd_settings_load(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    enum settings_result result = user_settings_load();
    if (result == SETTINGS_OK)
    {
        shell_print(sh, "Settings load initiated");
    }
    else
    {
        shell_error(sh, "Failed to initiate settings load: %d", result);
    }

    return 0;
}

static int cmd_settings_reset(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "confirm") != 0)
    {
        shell_print(sh, "Are you sure? This will reset all settings to defaults.");
        shell_print(sh, "Type 'settings reset confirm' to proceed.");
        return 0;
    }

    enum settings_result result = user_settings_reset();
    if (result == SETTINGS_OK)
    {
        shell_print(sh, "Settings reset to defaults");
    }
    else
    {
        shell_error(sh, "Failed to reset settings: %d", result);
    }

    return 0;
}

static int cmd_settings_json(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    char json_buffer[256]; /* Reduced buffer size */
    int len = user_settings_to_json(json_buffer, sizeof(json_buffer));

    if (len > 0)
    {
        shell_print(sh, "%s", json_buffer);
    }
    else
    {
        shell_error(sh, "Failed to generate JSON: %d", len);
    }

    return 0;
}

static int cmd_settings_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "Settings Status:");
    shell_print(sh, "  Dirty: %s", atomic_test_bit(&settings_dirty, 0) ? "Yes" : "No");

    /* Count callbacks */
    k_mutex_lock(&callback_mutex, K_FOREVER);
    int callback_count = 0;
    struct callback_node *node = callback_list;
    while (node)
    {
        callback_count++;
        node = node->next;
    }
    k_mutex_unlock(&callback_mutex);

    shell_print(sh, "  Callbacks registered: %d", callback_count);

    return 0;
}

/* Shell command registration */
SHELL_STATIC_SUBCMD_SET_CREATE(settings_cmds,
                               SHELL_CMD(show, NULL, "Show current settings", cmd_settings_show),
                               SHELL_CMD(set_device_id, NULL, "Set device ID", cmd_settings_set_device_id),
                               SHELL_CMD(set_wifi, NULL, "Set WiFi credentials", cmd_settings_set_wifi),
                               SHELL_CMD(wifi_enable, NULL, "Enable/disable WiFi", cmd_settings_wifi_enable),
                               SHELL_CMD(save, NULL, "Save settings to storage", cmd_settings_save),
                               SHELL_CMD(load, NULL, "Load settings from storage", cmd_settings_load),
                               SHELL_CMD(reset, NULL, "Reset settings to defaults", cmd_settings_reset),
                               SHELL_CMD(json, NULL, "Show settings as JSON", cmd_settings_json),
                               SHELL_CMD(status, NULL, "Show settings status", cmd_settings_status),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(settings, &settings_cmds, "Settings management", NULL);