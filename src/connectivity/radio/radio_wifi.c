/**
 * @file radio_wifi.c
 * @brief WiFi Radio Backend for Radio Abstraction Layer
 *
 * Implements RAL interface for IEEE 802.11 WiFi radios (ESP32, nRF7002, etc.)
 * Binds to Zephyr's WiFi management API.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
*/

#include "connectivity/radio_interface.h"
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <string.h>

LOG_MODULE_REGISTER(radio_wifi, CONFIG_AKIRA_LOG_LEVEL);

#ifdef CONFIG_WIFI

/* WiFi radio private data */
struct wifi_radio_data {
    struct net_if *iface;
    struct net_mgmt_event_callback mgmt_cb;
    struct k_sem scan_sem;
    radio_stats_t stats;
};

static struct wifi_radio_data wifi_data;
static radio_handle_t wifi_handle;

/* WiFi management event handler */
static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
                                   uint32_t mgmt_event, struct net_if *iface)
{
    struct wifi_radio_data *data = CONTAINER_OF(cb, struct wifi_radio_data, mgmt_cb);
    
    switch (mgmt_event) {
    case NET_EVENT_WIFI_SCAN_DONE:
        LOG_DBG("WiFi scan completed");
        k_sem_give(&data->scan_sem);
        
        /* Notify event callback if registered */
        if (wifi_handle.event_cb) {
            radio_event_t event = {
                .type = RADIO_EVENT_SCAN_DONE,
                .user_data = wifi_handle.event_user_data,
            };
            wifi_handle.event_cb(&event, wifi_handle.event_user_data);
        }
        break;
        
    case NET_EVENT_WIFI_CONNECT_RESULT:
        LOG_INF("WiFi connected");
        data->stats.rx_packets = 0;  /* Reset stats on reconnect */
        data->stats.tx_packets = 0;
        
        if (wifi_handle.event_cb) {
            radio_event_t event = {
                .type = RADIO_EVENT_CONNECTED,
                .user_data = wifi_handle.event_user_data,
            };
            wifi_handle.event_cb(&event, wifi_handle.event_user_data);
        }
        break;
        
    case NET_EVENT_WIFI_DISCONNECT_RESULT:
        LOG_INF("WiFi disconnected");
        
        if (wifi_handle.event_cb) {
            radio_event_t event = {
                .type = RADIO_EVENT_DISCONNECTED,
                .user_data = wifi_handle.event_user_data,
            };
            wifi_handle.event_cb(&event, wifi_handle.event_user_data);
        }
        break;
        
    default:
        break;
    }
}

/* RAL operation implementations */

static int wifi_radio_init(radio_handle_t *handle)
{
    struct wifi_radio_data *data = handle->priv_data;
    
    /* Get default network interface */
    data->iface = net_if_get_default();
    if (!data->iface) {
        LOG_ERR("No default network interface found");
        return -ENODEV;
    }
    
    /* Initialize semaphore for scan operations */
    k_sem_init(&data->scan_sem, 0, 1);
    
    /* Register for WiFi management events */
    net_mgmt_init_event_callback(&data->mgmt_cb, wifi_mgmt_event_handler,
                                NET_EVENT_WIFI_SCAN_DONE |
                                NET_EVENT_WIFI_CONNECT_RESULT |
                                NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&data->mgmt_cb);
    
    LOG_INF("WiFi radio initialized");
    handle->state = RADIO_STATE_IDLE;
    
    return 0;
}

static int wifi_radio_deinit(radio_handle_t *handle)
{
    struct wifi_radio_data *data = handle->priv_data;
    
    /* Unregister management events */
    net_mgmt_del_event_callback(&data->mgmt_cb);
    
    handle->state = RADIO_STATE_OFF;
    LOG_INF("WiFi radio deinitialized");
    
    return 0;
}

static int wifi_radio_configure(radio_handle_t *handle, const radio_config_t *config)
{
    /* WiFi configuration is typically done through WiFi-specific APIs */
    /* This is a placeholder for generic radio configuration */
    LOG_DBG("WiFi radio configuration (channel=%d, power=%d dBm)", 
            config->channel, config->tx_power);
    
    return 0;
}

static int wifi_radio_get_config(radio_handle_t *handle, radio_config_t *config)
{
    struct wifi_radio_data *data = handle->priv_data;
    struct wifi_iface_status status;
    
    int ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, data->iface, 
                      &status, sizeof(status));
    if (ret) {
        return ret;
    }
    
    memset(config, 0, sizeof(*config));
    config->channel = status.channel;
    config->mtu = net_if_get_mtu(data->iface);
    
    return 0;
}

static int wifi_radio_send(radio_handle_t *handle, const uint8_t *data, size_t len)
{
    struct wifi_radio_data *radio_data = handle->priv_data;
    
    /* WiFi TX is typically done through sockets, not raw radio interface */
    /* This would require deeper integration with net stack */
    LOG_WRN("WiFi raw send not implemented - use sockets instead");
    
    radio_data->stats.tx_packets++;
    radio_data->stats.tx_bytes += len;
    
    return -ENOTSUP;
}

static int wifi_radio_recv(radio_handle_t *handle, uint8_t *buf, size_t buf_len, 
                          uint32_t timeout_ms)
{
    /* WiFi RX is typically done through sockets, not raw radio interface */
    LOG_WRN("WiFi raw recv not implemented - use sockets instead");
    return -ENOTSUP;
}

static int wifi_radio_scan(radio_handle_t *handle, uint32_t timeout_ms)
{
    struct wifi_radio_data *data = handle->priv_data;
    
    LOG_INF("Starting WiFi scan (timeout=%u ms)", timeout_ms);
    
    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, data->iface, NULL, 0);
    if (ret) {
        LOG_ERR("WiFi scan request failed: %d", ret);
        return ret;
    }
    
    handle->state = RADIO_STATE_SCAN;
    
    /* Wait for scan completion */
    ret = k_sem_take(&data->scan_sem, K_MSEC(timeout_ms));
    if (ret) {
        LOG_WRN("WiFi scan timeout");
        handle->state = RADIO_STATE_IDLE;
        return -ETIMEDOUT;
    }
    
    handle->state = RADIO_STATE_IDLE;
    return 0;
}

static int wifi_radio_set_state(radio_handle_t *handle, radio_state_t state)
{
    LOG_DBG("WiFi radio set state: %s", radio_state_to_string(state));
    handle->state = state;
    return 0;
}

static radio_state_t wifi_radio_get_state(radio_handle_t *handle)
{
    return handle->state;
}

static int wifi_radio_get_stats(radio_handle_t *handle, radio_stats_t *stats)
{
    struct wifi_radio_data *data = handle->priv_data;
    struct wifi_iface_status status;
    
    int ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, data->iface, 
                      &status, sizeof(status));
    if (ret == 0) {
        data->stats.rssi = status.rssi;
    }
    
    memcpy(stats, &data->stats, sizeof(*stats));
    return 0;
}

static int wifi_radio_reset(radio_handle_t *handle)
{
    LOG_WRN("WiFi radio reset requested");
    /* Reset would require driver-specific implementation */
    return -ENOTSUP;
}

static int wifi_radio_set_event_callback(radio_handle_t *handle, 
                                        radio_event_cb_t callback,
                                        void *user_data)
{
    handle->event_cb = callback;
    handle->event_user_data = user_data;
    LOG_DBG("WiFi event callback registered");
    return 0;
}

static int wifi_radio_get_hw_addr(radio_handle_t *handle, uint8_t *addr, size_t *addr_len)
{
    struct wifi_radio_data *data = handle->priv_data;
    struct net_linkaddr *link_addr;
    
    if (!addr || !addr_len) {
        return -EINVAL;
    }
    
    link_addr = net_if_get_link_addr(data->iface);
    if (!link_addr || !link_addr->addr) {
        return -ENOENT;
    }
    
    size_t copy_len = MIN(*addr_len, link_addr->len);
    memcpy(addr, link_addr->addr, copy_len);
    *addr_len = copy_len;
    
    return 0;
}

/* WiFi radio operations vtable */
static const radio_ops_t wifi_radio_ops = {
    .init = wifi_radio_init,
    .deinit = wifi_radio_deinit,
    .configure = wifi_radio_configure,
    .get_config = wifi_radio_get_config,
    .send = wifi_radio_send,
    .recv = wifi_radio_recv,
    .scan = wifi_radio_scan,
    .set_state = wifi_radio_set_state,
    .get_state = wifi_radio_get_state,
    .get_stats = wifi_radio_get_stats,
    .reset = wifi_radio_reset,
    .set_event_callback = wifi_radio_set_event_callback,
    .get_hw_addr = wifi_radio_get_hw_addr,
};

/* Initialize and register WiFi radio */
int radio_wifi_register(void)
{
    memset(&wifi_handle, 0, sizeof(wifi_handle));
    memset(&wifi_data, 0, sizeof(wifi_data));
    
    wifi_handle.type = RADIO_TYPE_WIFI;
    wifi_handle.name = "WiFi";
    wifi_handle.capabilities = RADIO_CAP_TX | RADIO_CAP_RX | RADIO_CAP_SCAN |
                              RADIO_CAP_ENCRYPTION | RADIO_CAP_MULTICAST;
    wifi_handle.ops = &wifi_radio_ops;
    wifi_handle.priv_data = &wifi_data;
    wifi_handle.state = RADIO_STATE_OFF;
    
    /* Initialize the radio */
    int ret = wifi_radio_init(&wifi_handle);
    if (ret) {
        LOG_ERR("WiFi radio initialization failed: %d", ret);
        return ret;
    }
    
    /* Register with radio manager */
    ret = radio_manager_register(&wifi_handle);
    if (ret) {
        LOG_ERR("Failed to register WiFi radio: %d", ret);
        return ret;
    }
    
    LOG_INF("WiFi radio registered successfully");
    return 0;
}

#endif /* CONFIG_WIFI */
