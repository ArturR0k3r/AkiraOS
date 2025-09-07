/**
 * @file settings.c
 * @brief User settings management module implementation
 *
 * Provides thread-safe persistent settings storage with change callbacks
 * and shell command integration for the Akira Board OTA system.
 */

#include "settings.h"
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/base64.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(user_settings, LOG_LEVEL_INF);

/* Thread stack and control block */
static K_THREAD_STACK_DEFINE(settings_thread_stack, SETTINGS_THREAD_STACK_SIZE);
static struct k_thread settings_thread_data;
static k_tid_t settings_thread_id;

/* Settings storage */
static struct user_settings current_settings;
static K_MUTEX_DEFINE(settings_mutex);

/* Default settings */
static const struct user_settings default_settings = {
    .device_id = "akira-default",
    .wifi_ssid = "",
    .wifi_passcode = "",
    .wifi_enabled = false};

/* Change callback */
static settings_change_cb_t change_callback = NULL;
static void *callback_user_data = NULL;

/* Message queue for settings operations */
#define SETTINGS_MSG_QUEUE_SIZE 10

enum settings_msg_type
{
    MSG_SAVE_SETTINGS,
    MSG_LOAD_SETTINGS,
    MSG_RESET_SETTINGS,
    MSG_SET_DEVICE_ID,
    MSG_SET_WIFI_CREDENTIALS,
    MSG_SET_WIFI_ENABLED
};

struct settings_msg
{
    enum settings_msg_type type;
    union
    {
        struct
        {
            char device_id[MAX_DEVICE_ID_LEN];
        } set_device_id;
        struct
        {
            char ssid[MAX_WIFI_SSID_LEN];
            char passcode[MAX_WIFI_PASSCODE_LEN];
        } set_wifi;
        struct
        {
            bool enabled;
        } set_wifi_enabled;
    } data;
    struct k_sem *completion_sem;
    enum settings_result *result;
};

K_MSGQ_DEFINE(settings_msgq, sizeof(struct settings_msg), SETTINGS_MSG_QUEUE_SIZE, 4);

/* Settings handlers for Zephyr settings subsystem */
static int settings_set_handler(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
    int ret;

    if (strcmp(key, "device/id") == 0)
    {
        ret = read_cb(cb_arg, current_settings.device_id, sizeof(current_settings.device_id));
        if (ret > 0)
        {
            current_settings.device_id[ret] = '\0';
            LOG_DBG("Loaded device ID: %s", current_settings.device_id);
        }
    }
    else if (strcmp(key, "wifi/ssid") == 0)
    {
        ret = read_cb(cb_arg, current_settings.wifi_ssid, sizeof(current_settings.wifi_ssid));
        if (ret > 0)
        {
            current_settings.wifi_ssid[ret] = '\0';
            LOG_DBG("Loaded WiFi SSID: %s", current_settings.wifi_ssid);
        }
    }
    else if (strcmp(key, "wifi/passcode") == 0)
    {
        ret = read_cb(cb_arg, current_settings.wifi_passcode, sizeof(current_settings.wifi_passcode));
        if (ret > 0)
        {
            current_settings.wifi_passcode[ret] = '\0';
            LOG_DBG("Loaded WiFi passcode (hidden)");
        }
    }
    else if (strcmp(key, "wifi/enabled") == 0)
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
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(user_settings, NULL, NULL, settings_set_handler, settings_commit_handler, NULL);

/* Internal helper functions */
static enum settings_result save_setting(const char *key, const void *value, size_t len)
{
    int ret = settings_save_one(key, value, len);
    if (ret != 0)
    {
        LOG_ERR("Failed to save setting %s: %d", key, ret);
        return SETTINGS_SAVE_FAILED;
    }
    LOG_DBG("Saved setting: %s", key);
    return SETTINGS_OK;
}

static void notify_change(const char *key, const void *value)
{
    if (change_callback)
    {
        change_callback(key, value, callback_user_data);
    }
}

/* Settings operations (run in settings thread) */
static enum settings_result do_save_settings(void)
{
    enum settings_result result = SETTINGS_OK;

    k_mutex_lock(&settings_mutex, K_FOREVER);

    if (save_setting(DEVICE_ID_KEY, current_settings.device_id, strlen(current_settings.device_id)) != SETTINGS_OK)
    {
        result = SETTINGS_SAVE_FAILED;
    }

    if (save_setting(WIFI_SSID_KEY, current_settings.wifi_ssid, strlen(current_settings.wifi_ssid)) != SETTINGS_OK)
    {
        result = SETTINGS_SAVE_FAILED;
    }

    if (save_setting(WIFI_PASSCODE_KEY, current_settings.wifi_passcode, strlen(current_settings.wifi_passcode)) != SETTINGS_OK)
    {
        result = SETTINGS_SAVE_FAILED;
    }

    uint8_t enabled = current_settings.wifi_enabled ? 1 : 0;
    if (save_setting(WIFI_ENABLED_KEY, &enabled, sizeof(enabled)) != SETTINGS_OK)
    {
        result = SETTINGS_SAVE_FAILED;
    }

    k_mutex_unlock(&settings_mutex);

    if (result == SETTINGS_OK)
    {
        LOG_INF("All settings saved successfully");
    }
    else
    {
        LOG_ERR("Failed to save some settings");
    }

    return result;
}

static enum settings_result do_load_settings(void)
{
    int ret = settings_load();
    if (ret != 0)
    {
        LOG_ERR("Failed to load settings: %d", ret);
        return SETTINGS_ERROR;
    }

    LOG_INF("Settings loaded successfully");
    return SETTINGS_OK;
}

static enum settings_result do_reset_settings(void)
{
    k_mutex_lock(&settings_mutex, K_FOREVER);
    memcpy(&current_settings, &default_settings, sizeof(current_settings));
    k_mutex_unlock(&settings_mutex);

    enum settings_result result = do_save_settings();
    if (result == SETTINGS_OK)
    {
        LOG_INF("Settings reset to defaults");
        notify_change("*", NULL); /* Notify all settings changed */
    }

    return result;
}

static enum settings_result do_set_device_id(const char *device_id)
{
    if (!device_id || strlen(device_id) >= MAX_DEVICE_ID_LEN)
    {
        return SETTINGS_INVALID_PARAM;
    }

    k_mutex_lock(&settings_mutex, K_FOREVER);
    strcpy(current_settings.device_id, device_id);
    k_mutex_unlock(&settings_mutex);

    enum settings_result result = save_setting(DEVICE_ID_KEY, device_id, strlen(device_id));
    if (result == SETTINGS_OK)
    {
        LOG_INF("Device ID updated: %s", device_id);
        notify_change(DEVICE_ID_KEY, device_id);
    }

    return result;
}

static enum settings_result do_set_wifi_credentials(const char *ssid, const char *passcode)
{
    if (!ssid || !passcode ||
        strlen(ssid) >= MAX_WIFI_SSID_LEN ||
        strlen(passcode) >= MAX_WIFI_PASSCODE_LEN)
    {
        return SETTINGS_INVALID_PARAM;
    }

    k_mutex_lock(&settings_mutex, K_FOREVER);
    strcpy(current_settings.wifi_ssid, ssid);
    strcpy(current_settings.wifi_passcode, passcode);
    k_mutex_unlock(&settings_mutex);

    enum settings_result result = SETTINGS_OK;

    if (save_setting(WIFI_SSID_KEY, ssid, strlen(ssid)) != SETTINGS_OK)
    {
        result = SETTINGS_SAVE_FAILED;
    }

    if (save_setting(WIFI_PASSCODE_KEY, passcode, strlen(passcode)) != SETTINGS_OK)
    {
        result = SETTINGS_SAVE_FAILED;
    }

    if (result == SETTINGS_OK)
    {
        LOG_INF("WiFi credentials updated: %s", ssid);
        notify_change(WIFI_SSID_KEY, ssid);
        notify_change(WIFI_PASSCODE_KEY, passcode);
    }

    return result;
}

static enum settings_result do_set_wifi_enabled(bool enabled)
{
    k_mutex_lock(&settings_mutex, K_FOREVER);
    current_settings.wifi_enabled = enabled;
    k_mutex_unlock(&settings_mutex);

    uint8_t enabled_val = enabled ? 1 : 0;
    enum settings_result result = save_setting(WIFI_ENABLED_KEY, &enabled_val, sizeof(enabled_val));

    if (result == SETTINGS_OK)
    {
        LOG_INF("WiFi enabled: %s", enabled ? "true" : "false");
        notify_change(WIFI_ENABLED_KEY, &enabled);
    }

    return result;
}

/* Settings thread main function */
static void settings_thread_main(void *p1, void *p2, void *p3)
{
    struct settings_msg msg;

    LOG_INF("Settings thread started");

    while (1)
    {
        if (k_msgq_get(&settings_msgq, &msg, K_FOREVER) == 0)
        {
            enum settings_result result = SETTINGS_ERROR;

            switch (msg.type)
            {
            case MSG_SAVE_SETTINGS:
                result = do_save_settings();
                break;

            case MSG_LOAD_SETTINGS:
                result = do_load_settings();
                break;

            case MSG_RESET_SETTINGS:
                result = do_reset_settings();
                break;

            case MSG_SET_DEVICE_ID:
                result = do_set_device_id(msg.data.set_device_id.device_id);
                break;

            case MSG_SET_WIFI_CREDENTIALS:
                result = do_set_wifi_credentials(msg.data.set_wifi.ssid, msg.data.set_wifi.passcode);
                break;

            case MSG_SET_WIFI_ENABLED:
                result = do_set_wifi_enabled(msg.data.set_wifi_enabled.enabled);
                break;
            }

            /* Signal completion if requested */
            if (msg.result)
            {
                *msg.result = result;
            }
            if (msg.completion_sem)
            {
                k_sem_give(msg.completion_sem);
            }
        }
    }
}

/* Helper function to send message and wait for completion */
static enum settings_result send_settings_message(struct settings_msg *msg)
{
    enum settings_result result;
    struct k_sem completion_sem;

    k_sem_init(&completion_sem, 0, 1);
    msg->completion_sem = &completion_sem;
    msg->result = &result;

    if (k_msgq_put(&settings_msgq, msg, K_NO_WAIT) != 0)
    {
        LOG_ERR("Settings message queue full");
        return SETTINGS_ERROR;
    }

    k_sem_take(&completion_sem, K_FOREVER);
    return result;
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

    /* Start settings thread */
    settings_thread_id = k_thread_create(&settings_thread_data,
                                         settings_thread_stack,
                                         K_THREAD_STACK_SIZEOF(settings_thread_stack),
                                         settings_thread_main,
                                         NULL, NULL, NULL,
                                         SETTINGS_THREAD_PRIORITY,
                                         0, K_NO_WAIT);

    k_thread_name_set(settings_thread_id, "settings");

    /* Load existing settings */
    user_settings_load();

    LOG_INF("User settings module initialized");
    return 0;
}

const struct user_settings *user_settings_get(void)
{
    return &current_settings;
}

enum settings_result user_settings_set_device_id(const char *device_id)
{
    struct settings_msg msg = {
        .type = MSG_SET_DEVICE_ID};

    if (!device_id || strlen(device_id) >= MAX_DEVICE_ID_LEN)
    {
        return SETTINGS_INVALID_PARAM;
    }

    strcpy(msg.data.set_device_id.device_id, device_id);
    return send_settings_message(&msg);
}

enum settings_result user_settings_set_wifi_credentials(const char *ssid, const char *passcode)
{
    struct settings_msg msg = {
        .type = MSG_SET_WIFI_CREDENTIALS};

    if (!ssid || !passcode ||
        strlen(ssid) >= MAX_WIFI_SSID_LEN ||
        strlen(passcode) >= MAX_WIFI_PASSCODE_LEN)
    {
        return SETTINGS_INVALID_PARAM;
    }

    strcpy(msg.data.set_wifi.ssid, ssid);
    strcpy(msg.data.set_wifi.passcode, passcode);
    return send_settings_message(&msg);
}

enum settings_result user_settings_set_wifi_enabled(bool enabled)
{
    struct settings_msg msg = {
        .type = MSG_SET_WIFI_ENABLED,
        .data.set_wifi_enabled.enabled = enabled};

    return send_settings_message(&msg);
}

enum settings_result user_settings_save(void)
{
    struct settings_msg msg = {
        .type = MSG_SAVE_SETTINGS};

    return send_settings_message(&msg);
}

enum settings_result user_settings_load(void)
{
    struct settings_msg msg = {
        .type = MSG_LOAD_SETTINGS};

    return send_settings_message(&msg);
}

enum settings_result user_settings_reset(void)
{
    struct settings_msg msg = {
        .type = MSG_RESET_SETTINGS};

    return send_settings_message(&msg);
}

enum settings_result user_settings_register_callback(settings_change_cb_t callback, void *user_data)
{
    change_callback = callback;
    callback_user_data = user_data;
    return SETTINGS_OK;
}

void user_settings_print(void)
{
    k_mutex_lock(&settings_mutex, K_FOREVER);

    printk("\n=== User Settings ===\n");
    printk("Device ID: %s\n", current_settings.device_id);
    printk("WiFi SSID: %s\n", current_settings.wifi_ssid);
    printk("WiFi Enabled: %s\n", current_settings.wifi_enabled ? "Yes" : "No");
    printk("WiFi Passcode: %s\n", strlen(current_settings.wifi_passcode) > 0 ? "***SET***" : "***NOT SET***");

    k_mutex_unlock(&settings_mutex);
}

int user_settings_to_json(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size < 100)
    {
        return SETTINGS_BUFFER_TOO_SMALL;
    }

    k_mutex_lock(&settings_mutex, K_FOREVER);

    int len = snprintf(buffer, buffer_size,
                       "{\n"
                       "  \"device_id\": \"%s\",\n"
                       "  \"wifi_ssid\": \"%s\",\n"
                       "  \"wifi_enabled\": %s,\n"
                       "  \"wifi_passcode_set\": %s\n"
                       "}",
                       current_settings.device_id,
                       current_settings.wifi_ssid,
                       current_settings.wifi_enabled ? "true" : "false",
                       strlen(current_settings.wifi_passcode) > 0 ? "true" : "false");

    k_mutex_unlock(&settings_mutex);

    return (len < buffer_size) ? len : SETTINGS_BUFFER_TOO_SMALL;
}

/* Shell commands */
static int cmd_settings_show(const struct shell *sh, size_t argc, char **argv)
{
    const struct user_settings *settings = user_settings_get();

    shell_print(sh, "\n=== Current Settings ===");
    shell_print(sh, "Device ID: %s", settings->device_id);
    shell_print(sh, "WiFi SSID: %s", settings->wifi_ssid);
    shell_print(sh, "WiFi Enabled: %s", settings->wifi_enabled ? "Yes" : "No");
    shell_print(sh, "WiFi Passcode: %s",
                strlen(settings->wifi_passcode) > 0 ? "***SET***" : "***NOT SET***");

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
    if (result == SETTINGS_OK)
    {
        shell_print(sh, "Device ID set to: %s", argv[1]);
    }
    else
    {
        shell_error(sh, "Failed to set device ID: %d", result);
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
    if (result == SETTINGS_OK)
    {
        shell_print(sh, "WiFi credentials updated for SSID: %s", argv[1]);
    }
    else
    {
        shell_error(sh, "Failed to set WiFi credentials: %d", result);
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
    enum settings_result result = user_settings_save();
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
    enum settings_result result = user_settings_load();
    if (result == SETTINGS_OK)
    {
        shell_print(sh, "Settings loaded successfully");
    }
    else
    {
        shell_error(sh, "Failed to load settings: %d", result);
    }

    return 0;
}

static int cmd_settings_reset(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Are you sure? This will reset all settings to defaults.");
    shell_print(sh, "Type 'settings reset confirm' to proceed.");

    if (argc > 1 && strcmp(argv[1], "confirm") == 0)
    {
        enum settings_result result = user_settings_reset();
        if (result == SETTINGS_OK)
        {
            shell_print(sh, "Settings reset to defaults");
        }
        else
        {
            shell_error(sh, "Failed to reset settings: %d", result);
        }
    }

    return 0;
}

static int cmd_settings_json(const struct shell *sh, size_t argc, char **argv)
{
    char json_buffer[512];
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
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(settings, &settings_cmds, "Settings management", NULL);