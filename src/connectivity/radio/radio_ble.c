/**
 * @file radio_ble.c
 * @brief BLE Radio Backend for Radio Abstraction Layer
 *
 * Implements RAL interface for Bluetooth Low Energy radios.
 * Integrates with existing BT manager and provides raw HCI access.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
*/

#include "connectivity/radio_interface.h"
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <string.h>

LOG_MODULE_REGISTER(radio_ble, CONFIG_AKIRA_LOG_LEVEL);

#ifdef CONFIG_BT

/* BLE radio private data */
struct ble_radio_data {
    struct bt_le_scan_cb scan_cb;
    struct k_sem scan_sem;
    radio_stats_t stats;
    uint8_t hw_addr[6];
    bool initialized;
};

static struct ble_radio_data ble_data;
static radio_handle_t ble_handle;

/* BLE scan callback */
static void ble_scan_recv(const struct bt_le_scan_recv_info *info,
                         struct net_buf_simple *buf)
{
    ble_data.stats.rx_packets++;
    ble_data.stats.rx_bytes += buf->len;
    ble_data.stats.rssi = info->rssi;
    
    /* Notify event callback if registered */
    if (ble_handle.event_cb) {
        radio_event_t event = {
            .type = RADIO_EVENT_RX_DONE,
            .data = buf->data,
            .len = buf->len,
            .rssi = info->rssi,
            .user_data = ble_handle.event_user_data,
        };
        ble_handle.event_cb(&event, ble_handle.event_user_data);
    }
}

static void ble_scan_timeout(void)
{
    LOG_DBG("BLE scan timeout");
    k_sem_give(&ble_data.scan_sem);
    
    if (ble_handle.event_cb) {
        radio_event_t event = {
            .type = RADIO_EVENT_SCAN_DONE,
            .user_data = ble_handle.event_user_data,
        };
        ble_handle.event_cb(&event, ble_handle.event_user_data);
    }
}

/* RAL operation implementations */

static int ble_radio_init(radio_handle_t *handle)
{
    struct ble_radio_data *data = handle->priv_data;
    
    if (data->initialized) {
        LOG_WRN("BLE radio already initialized");
        return 0;
    }
    
    /* Enable Bluetooth (if not already enabled) */
    int ret = bt_enable(NULL);
    if (ret && ret != -EALREADY) {
        LOG_ERR("Bluetooth enable failed: %d", ret);
        return ret;
    }
    
    /* Get local Bluetooth address */
    bt_addr_le_t addr;
    size_t count = 1;
    bt_id_get(&addr, &count);
    memcpy(data->hw_addr, addr.a.val, 6);
    
    /* Initialize semaphore for scan operations */
    k_sem_init(&data->scan_sem, 0, 1);
    
    /* Register scan callbacks */
    data->scan_cb.recv = ble_scan_recv;
    data->scan_cb.timeout = ble_scan_timeout;
    bt_le_scan_cb_register(&data->scan_cb);
    
    data->initialized = true;
    handle->state = RADIO_STATE_IDLE;
    
    LOG_INF("BLE radio initialized - Address: %02x:%02x:%02x:%02x:%02x:%02x",
            data->hw_addr[5], data->hw_addr[4], data->hw_addr[3],
            data->hw_addr[2], data->hw_addr[1], data->hw_addr[0]);
    
    return 0;
}

static int ble_radio_deinit(radio_handle_t *handle)
{
    struct ble_radio_data *data = handle->priv_data;
    
    /* Unregister scan callbacks */
    bt_le_scan_cb_unregister(&data->scan_cb);
    
    /* Stop scanning if active */
    bt_le_scan_stop();
    
    data->initialized = false;
    handle->state = RADIO_STATE_OFF;
    
    LOG_INF("BLE radio deinitialized");
    return 0;
}

static int ble_radio_configure(radio_handle_t *handle, const radio_config_t *config)
{
    /* BLE configuration (tx_power, scan params, etc.) */
    LOG_DBG("BLE radio configuration (power=%d dBm)", config->tx_power);
    
    /* Set TX power if supported */
    #ifdef CONFIG_BT_CTLR_TX_PWR_DYNAMIC_CONTROL
    /* Note: Actual TX power setting requires controller-specific API */
    #endif
    
    return 0;
}

static int ble_radio_get_config(radio_handle_t *handle, radio_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->channel = 37;  /* BLE advertising channel 37 (2402 MHz) */
    config->mtu = 251;     /* BLE 4.2+ MTU */
    config->tx_power = 0;  /* Default 0 dBm */
    
    return 0;
}

static int ble_radio_send(radio_handle_t *handle, const uint8_t *data, size_t len)
{
    struct ble_radio_data *radio_data = handle->priv_data;
    
    /* BLE TX typically done through advertising or GATT */
    /* Raw HCI send would require deeper integration */
    LOG_WRN("BLE raw send not fully implemented - use BLE advertising/GATT");
    
    radio_data->stats.tx_packets++;
    radio_data->stats.tx_bytes += len;
    
    return -ENOTSUP;
}

static int ble_radio_recv(radio_handle_t *handle, uint8_t *buf, size_t buf_len,
                         uint32_t timeout_ms)
{
    /* BLE RX handled through scan callbacks */
    LOG_WRN("BLE raw recv not implemented - use scan callbacks");
    return -ENOTSUP;
}

static int ble_radio_scan(radio_handle_t *handle, uint32_t timeout_ms)
{
    struct ble_radio_data *data = handle->priv_data;
    
    LOG_INF("Starting BLE scan (timeout=%u ms)", timeout_ms);
    
    /* Configure scan parameters */
    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };
    
    int ret = bt_le_scan_start(&scan_param, NULL);
    if (ret && ret != -EALREADY) {
        LOG_ERR("BLE scan start failed: %d", ret);
        return ret;
    }
    
    handle->state = RADIO_STATE_SCAN;
    
    /* Wait for scan timeout or manual stop */
    if (timeout_ms > 0) {
        k_sleep(K_MSEC(timeout_ms));
        bt_le_scan_stop();
    } else {
        /* Infinite scan - wait for explicit stop */
        ret = k_sem_take(&data->scan_sem, K_FOREVER);
    }
    
    handle->state = RADIO_STATE_IDLE;
    return 0;
}

static int ble_radio_set_state(radio_handle_t *handle, radio_state_t state)
{
    LOG_DBG("BLE radio set state: %s", radio_state_to_string(state));
    
    switch (state) {
    case RADIO_STATE_IDLE:
        bt_le_scan_stop();
        break;
    case RADIO_STATE_SCAN:
        /* Scan started via ble_radio_scan() */
        break;
    case RADIO_STATE_SLEEP:
        /* Low power mode - would require controller-specific API */
        bt_le_scan_stop();
        break;
    default:
        break;
    }
    
    handle->state = state;
    return 0;
}

static radio_state_t ble_radio_get_state(radio_handle_t *handle)
{
    return handle->state;
}

static int ble_radio_get_stats(radio_handle_t *handle, radio_stats_t *stats)
{
    struct ble_radio_data *data = handle->priv_data;
    memcpy(stats, &data->stats, sizeof(*stats));
    return 0;
}

static int ble_radio_reset(radio_handle_t *handle)
{
    LOG_WRN("BLE radio reset requested");
    
    /* Stop scanning */
    bt_le_scan_stop();
    
    /* Reset statistics */
    struct ble_radio_data *data = handle->priv_data;
    memset(&data->stats, 0, sizeof(data->stats));
    
    handle->state = RADIO_STATE_IDLE;
    return 0;
}

static int ble_radio_set_event_callback(radio_handle_t *handle,
                                       radio_event_cb_t callback,
                                       void *user_data)
{
    handle->event_cb = callback;
    handle->event_user_data = user_data;
    LOG_DBG("BLE event callback registered");
    return 0;
}

static int ble_radio_get_hw_addr(radio_handle_t *handle, uint8_t *addr, size_t *addr_len)
{
    struct ble_radio_data *data = handle->priv_data;
    
    if (!addr || !addr_len) {
        return -EINVAL;
    }
    
    size_t copy_len = MIN(*addr_len, 6);
    memcpy(addr, data->hw_addr, copy_len);
    *addr_len = copy_len;
    
    return 0;
}

/* BLE radio operations vtable */
static const radio_ops_t ble_radio_ops = {
    .init = ble_radio_init,
    .deinit = ble_radio_deinit,
    .configure = ble_radio_configure,
    .get_config = ble_radio_get_config,
    .send = ble_radio_send,
    .recv = ble_radio_recv,
    .scan = ble_radio_scan,
    .set_state = ble_radio_set_state,
    .get_state = ble_radio_get_state,
    .get_stats = ble_radio_get_stats,
    .reset = ble_radio_reset,
    .set_event_callback = ble_radio_set_event_callback,
    .get_hw_addr = ble_radio_get_hw_addr,
};

/* Initialize and register BLE radio */
int radio_ble_register(void)
{
    memset(&ble_handle, 0, sizeof(ble_handle));
    memset(&ble_data, 0, sizeof(ble_data));
    
    ble_handle.type = RADIO_TYPE_BLE;
    ble_handle.name = "BLE";
    ble_handle.capabilities = RADIO_CAP_TX | RADIO_CAP_RX | RADIO_CAP_SCAN |
                             RADIO_CAP_MESH | RADIO_CAP_ENCRYPTION |
                             RADIO_CAP_LOW_POWER | RADIO_CAP_MULTICAST;
    ble_handle.ops = &ble_radio_ops;
    ble_handle.priv_data = &ble_data;
    ble_handle.state = RADIO_STATE_OFF;
    
    /* Initialize the radio */
    int ret = ble_radio_init(&ble_handle);
    if (ret) {
        LOG_ERR("BLE radio initialization failed: %d", ret);
        return ret;
    }
    
    /* Register with radio manager */
    ret = radio_manager_register(&ble_handle);
    if (ret) {
        LOG_ERR("Failed to register BLE radio: %d", ret);
        return ret;
    }
    
    LOG_INF("BLE radio registered successfully");
    return 0;
}

#endif /* CONFIG_BT */
