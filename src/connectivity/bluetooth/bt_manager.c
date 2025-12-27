/**
 * @file bt_manager.c
 * @brief Bluetooth Manager Implementation for AkiraOS
 */

#include "bt_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#if defined(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/settings/settings.h>
#define BT_AVAILABLE 1
#else
#define BT_AVAILABLE 0
#endif

#if defined(CONFIG_AKIRA_BT_ECHO)
#include "bt_echo.h"
#endif

#ifndef CONFIG_BT_DEVICE_NAME_MAX
#define CONFIG_BT_DEVICE_NAME_MAX 65
#endif

LOG_MODULE_REGISTER(bt_manager, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    bt_config_t config;
    bt_state_t state;
    bt_stats_t stats;

#if BT_AVAILABLE
    struct bt_conn *current_conn;
#endif

    bt_event_callback_t event_cb;
    void *event_cb_data;

    /* Runtime device name storage (max from Kconfig) */
    char device_name[CONFIG_BT_DEVICE_NAME_MAX + 1];

    struct k_mutex mutex;
} bt_mgr;

/*===========================================================================*/
/* Name management API */
/*===========================================================================*/

int bt_manager_set_name(const char *name)
{
#if BT_AVAILABLE
    if (!name)
    {
        return -EINVAL;
    }

    size_t len = strlen(name);
    if (len >= CONFIG_BT_DEVICE_NAME_MAX)
    {
        return -EINVAL;
    }
#endif

    k_mutex_lock(&bt_mgr.mutex, K_FOREVER);
    strncpy(bt_mgr.device_name, name ? name : "", CONFIG_BT_DEVICE_NAME_MAX);
    bt_mgr.device_name[CONFIG_BT_DEVICE_NAME_MAX] = '\0';

#if BT_AVAILABLE
    /* If advertising, restart to apply new name */
    if (bt_mgr.state == BT_STATE_ADVERTISING)
    {
        bt_manager_stop_advertising();
        bt_manager_start_advertising();
    }
#endif
    k_mutex_unlock(&bt_mgr.mutex);

#if IS_ENABLED(CONFIG_SETTINGS)
    int rc = settings_save_one("bt/name", bt_mgr.device_name, strlen(bt_mgr.device_name) + 1);
    if (rc)
    {
        LOG_ERR("Failed to save BT name to settings (err %d)", rc);
    }
#endif

    LOG_INF("Bluetooth name set to: %s", bt_mgr.device_name);
    return 0;
}

int bt_manager_get_name(char *buffer, size_t len)
{
    if (!buffer || len == 0)
    {
        return -EINVAL;
    }

    k_mutex_lock(&bt_mgr.mutex, K_FOREVER);
    strncpy(buffer, bt_mgr.device_name, len - 1);
    buffer[len - 1] = '\0';
    k_mutex_unlock(&bt_mgr.mutex);
    return 0;
}

#if IS_ENABLED(CONFIG_SETTINGS)
/* Settings handler to persist bt/* subtree (currently handles "bt/name") */
static int bt_settings_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
    if (!key)
    {
        return -EINVAL;
    }

    if (strcmp(key, "name") == 0)
    {
        if (len == 0 || len > CONFIG_BT_DEVICE_NAME_MAX)
        {
            return -EINVAL;
        }

        char tmp[CONFIG_BT_DEVICE_NAME_MAX + 1] = {0};
        int rc = read_cb(cb_arg, tmp, len);
        if (rc < 0)
        {
            return rc;
        }
        tmp[len] = '\0';

        k_mutex_lock(&bt_mgr.mutex, K_FOREVER);
        strncpy(bt_mgr.device_name, tmp, CONFIG_BT_DEVICE_NAME_MAX);
        bt_mgr.device_name[CONFIG_BT_DEVICE_NAME_MAX] = '\0';
        bool was_adv = (bt_mgr.state == BT_STATE_ADVERTISING);
        k_mutex_unlock(&bt_mgr.mutex);

        if (was_adv)
        {
            bt_manager_stop_advertising();
            bt_manager_start_advertising();
        }

        LOG_INF("Loaded BT name from settings: %s", bt_mgr.device_name);
        return 0;
    }

    return -ENOENT;
}

static struct settings_handler bt_settings = {
    .name = "bt",
    .h_set = bt_settings_set,
};
#endif

/*===========================================================================*/
/* Internal Functions                                                        */
/*===========================================================================*/

static void notify_event(bt_event_t event, void *data)
{
    if (bt_mgr.event_cb)
    {
        bt_mgr.event_cb(event, data, bt_mgr.event_cb_data);
    }
}

#if BT_AVAILABLE

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
    if (err)
    {
        LOG_ERR("Connection failed (err 0x%02x)", err);
        bt_mgr.state = BT_STATE_READY;
        return;
    }

    bt_mgr.current_conn = bt_conn_ref(conn);
    bt_mgr.state = BT_STATE_CONNECTED;
    bt_mgr.stats.connections++;

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Connected: %s", addr);

    notify_event(BT_EVENT_CONNECTED, NULL);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Disconnected: %s (reason 0x%02x)", addr, reason);

    if (bt_mgr.current_conn)
    {
        bt_conn_unref(bt_mgr.current_conn);
        bt_mgr.current_conn = NULL;
    }

    bt_mgr.state = BT_STATE_READY;
    bt_mgr.stats.disconnections++;

    notify_event(BT_EVENT_DISCONNECTED, NULL);

    /* Auto-restart advertising if configured */
    if (bt_mgr.config.auto_advertise)
    {
        bt_manager_start_advertising();
    }

#if defined(CONFIG_AKIRA_BT_ECHO)
    bt_echo_init();
#endif
}

#if defined(CONFIG_BT_SMP) || defined(CONFIG_BT_CLASSIC)
static void security_changed_cb(struct bt_conn *conn, bt_security_t level,
                                enum bt_security_err err)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (err)
    {
        LOG_WRN("Security failed: %s level %d err %d", addr, level, err);
        return;
    }

    LOG_INF("Security changed: %s level %d", addr, level);

    if (level >= BT_SECURITY_L2)
    {
        bt_mgr.stats.bonded = true;
        notify_event(BT_EVENT_PAIRED, NULL);
    }
}
#endif /* CONFIG_BT_SMP || CONFIG_BT_CLASSIC */

static struct bt_conn_cb conn_callbacks = {
    .connected = connected_cb,
    .disconnected = disconnected_cb,
#if defined(CONFIG_BT_SMP) || defined(CONFIG_BT_CLASSIC)
    .security_changed = security_changed_cb,
#endif
};

/* Advertising data */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL,
                  BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
                  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

#endif /* BT_AVAILABLE */

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int bt_manager_init(const bt_config_t *config)
{
    if (bt_mgr.initialized)
    {
        return 0;
    }

    LOG_INF("Initializing Bluetooth manager");

    k_mutex_init(&bt_mgr.mutex);
    memset(&bt_mgr.stats, 0, sizeof(bt_stats_t));

    /* Apply configuration */
    if (config)
    {
        memcpy(&bt_mgr.config, config, sizeof(bt_config_t));
    }
    else
    {
        bt_mgr.config.device_name = CONFIG_BT_DEVICE_NAME;
        bt_mgr.config.vendor_id = 0x1234;
        bt_mgr.config.product_id = 0x5678;
        bt_mgr.config.services = BT_SERVICE_ALL;
        bt_mgr.config.auto_advertise = true;
        bt_mgr.config.pairable = true;
    }

    /* Initialize internal device_name buffer from config (truncate if needed) */
    strncpy(bt_mgr.device_name, bt_mgr.config.device_name ? bt_mgr.config.device_name : "", CONFIG_BT_DEVICE_NAME_MAX);
    bt_mgr.device_name[CONFIG_BT_DEVICE_NAME_MAX] = '\0';

    bt_mgr.state = BT_STATE_INITIALIZING;

#if BT_AVAILABLE
    int err = bt_enable(NULL);
    if (err)
    {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        bt_mgr.state = BT_STATE_ERROR;
        return err;
    }

    /* Load stored bonds and settings */
    if (IS_ENABLED(CONFIG_SETTINGS))
    {
        settings_register(&bt_settings);
        settings_load();
    }

    bt_conn_cb_register(&conn_callbacks);

    bt_mgr.state = BT_STATE_READY;
    bt_mgr.initialized = true;

    LOG_INF("Bluetooth initialized: %s", bt_mgr.config.device_name);

    notify_event(BT_EVENT_READY, NULL);

    if (bt_mgr.config.auto_advertise)
    {
        bt_manager_start_advertising();
    }

    return 0;
#else
    LOG_WRN("Bluetooth not available on this platform (simulation mode)");
    bt_mgr.state = BT_STATE_READY;
    bt_mgr.initialized = true;
    return 0;
#endif
}

int bt_manager_deinit(void)
{
    if (!bt_mgr.initialized)
    {
        return 0;
    }

    bt_manager_disconnect();
    bt_manager_stop_advertising();

    bt_mgr.initialized = false;
    bt_mgr.state = BT_STATE_OFF;

    LOG_INF("Bluetooth manager deinitialized");
    return 0;
}

int bt_manager_start_advertising(void)
{
#if BT_AVAILABLE
    if (!bt_mgr.initialized)
    {
        return -EINVAL;
    }

    if (bt_mgr.state == BT_STATE_CONNECTED)
    {
        return -EBUSY;
    }

    struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
        BT_LE_ADV_OPT_CONNECTABLE,
        BT_GAP_ADV_FAST_INT_MIN_2,
        BT_GAP_ADV_FAST_INT_MAX_2,
        NULL);

    struct bt_data sd[] = {
        BT_DATA(BT_DATA_NAME_COMPLETE, bt_mgr.device_name,
                strlen(bt_mgr.device_name)),
    };

    int err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err)
    {
        LOG_ERR("Advertising start failed (err %d)", err);
        return err;
    }

    bt_mgr.state = BT_STATE_ADVERTISING;
    LOG_INF("Bluetooth advertising started");
    return 0;
#else
    LOG_INF("Bluetooth advertising (simulated)");
    bt_mgr.state = BT_STATE_ADVERTISING;
    return 0;
#endif
}

int bt_manager_stop_advertising(void)
{
#if BT_AVAILABLE
    if (bt_mgr.state == BT_STATE_ADVERTISING)
    {
        bt_le_adv_stop();
        bt_mgr.state = BT_STATE_READY;
        LOG_INF("Bluetooth advertising stopped");
    }
    return 0;
#else
    bt_mgr.state = BT_STATE_READY;
    return 0;
#endif
}

int bt_manager_disconnect(void)
{
#if BT_AVAILABLE
    if (bt_mgr.current_conn)
    {
        bt_conn_disconnect(bt_mgr.current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
    return 0;
#else
    bt_mgr.state = BT_STATE_READY;
    notify_event(BT_EVENT_DISCONNECTED, NULL);
    return 0;
#endif
}

bt_state_t bt_manager_get_state(void)
{
    return bt_mgr.state;
}

int bt_manager_get_stats(bt_stats_t *stats)
{
    if (!stats)
    {
        return -EINVAL;
    }

    k_mutex_lock(&bt_mgr.mutex, K_FOREVER);
    memcpy(stats, &bt_mgr.stats, sizeof(bt_stats_t));
    stats->state = bt_mgr.state;
    k_mutex_unlock(&bt_mgr.mutex);

    return 0;
}

bool bt_manager_is_connected(void)
{
    return bt_mgr.state == BT_STATE_CONNECTED;
}

int bt_manager_register_callback(bt_event_callback_t callback, void *user_data)
{
    bt_mgr.event_cb = callback;
    bt_mgr.event_cb_data = user_data;
    return 0;
}

int bt_manager_unpair_all(void)
{
#if BT_AVAILABLE
    int err = bt_unpair(BT_ID_DEFAULT, NULL);
    if (err)
    {
        LOG_ERR("Failed to unpair (err %d)", err);
        return err;
    }
    LOG_INF("All bonds deleted");
    return 0;
#else
    return 0;
#endif
}

int bt_manager_get_address(char *buffer, size_t len)
{
    if (!buffer || len < 18)
    {
        return -EINVAL;
    }

#if BT_AVAILABLE
    bt_addr_le_t addr;
    size_t count = 1;
    bt_id_get(&addr, &count);
    bt_addr_le_to_str(&addr, buffer, len);
    return 0;
#else
    snprintf(buffer, len, "00:00:00:00:00:00");
    return 0;
#endif
}

/*===========================================================================*/
/* Shell Integration                                                         */
/*===========================================================================*/

void bluetooth_manager_receive_shell_command(const char *cmd)
{
    /* Stub for shell command reception via BLE
     * TODO: Implement BLE shell command passthrough
     */
    LOG_DBG("BLE shell command received: %s", cmd);
}
