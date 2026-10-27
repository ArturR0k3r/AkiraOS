/**
 * @file radio_802154.c
 * @brief IEEE 802.15.4 Radio Backend for Radio Abstraction Layer
 *
 * Implements RAL interface for 802.15.4 radios (nRF, CC2520, etc.)
 * Used for Thread, Zigbee, and custom mesh protocols.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
*/

#include "connectivity/radio_interface.h"
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ieee802154_radio.h>
#include <string.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/ieee802154_pkt.h>
#include <zephyr/net/promiscuous.h>

LOG_MODULE_REGISTER(radio_802154, CONFIG_AKIRA_LOG_LEVEL);

#if defined(CONFIG_NET_L2_IEEE802154)

/* 802.15.4 radio private data */
struct ieee802154_radio_data {
    struct net_if *iface;
    const struct device *radio_dev;
    struct ieee802154_radio_api *radio_api;
    radio_stats_t stats;
    uint8_t hw_addr[8];  /* IEEE 802.15.4 uses EUI-64 */
    uint16_t panid;
    uint16_t short_addr;
    bool initialized;
    /* Per-frame link metadata latched from the most recent recv() frame. */
    uint8_t last_lqi;    /* 0-255 LQI of last received frame            */
    int16_t last_rssi;   /* dBm RSSI of last frame (or DBM_UNDEFINED)   */
    bool have_rx_meta;   /* true once at least one frame has been RX'd  */
};

static struct ieee802154_radio_data ieee802154_data;
static radio_handle_t ieee802154_handle;

/* RAL operation implementations */

static int ieee802154_radio_init(radio_handle_t *handle)
{
    struct ieee802154_radio_data *data = handle->priv_data;
    
    if (data->initialized) {
        LOG_WRN("802.15.4 radio already initialized");
        return 0;
    }
    
    /* Get network interface for 802.15.4 */
    data->iface = net_if_get_first_by_type(&NET_L2_GET_NAME(IEEE802154));
    if (!data->iface) {
        LOG_ERR("No 802.15.4 network interface found");
        return -ENODEV;
    }
    
    /* Get radio device */
    data->radio_dev = net_if_get_device(data->iface);
    if (!data->radio_dev) {
        LOG_ERR("No radio device associated with interface");
        return -ENODEV;
    }
    
    /* Get hardware address (EUI-64) */
    struct net_linkaddr *link_addr = net_if_get_link_addr(data->iface);
    if (link_addr && link_addr->len == 8) {
        memcpy(data->hw_addr, link_addr->addr, 8);
    }
    
    data->panid = IEEE802154_BROADCAST_PAN_ID;
    data->short_addr = IEEE802154_SHORT_ADDRESS_NOT_ASSOCIATED;
    
    /* No RX frame captured yet: report LQI/RSSI as unavailable until recv()
     * latches real metadata from the first received frame. */
    data->have_rx_meta = false;
    data->last_lqi = 0;
    data->last_rssi = IEEE802154_MAC_RSSI_DBM_UNDEFINED;
    
#if defined(CONFIG_AKIRA_RADIO_802154_PROMISC_RX)
    /* Enable promiscuous capture so ieee802154_radio_recv() can pull raw
     * frames (with per-frame LQI/RSSI) off the net stack. Best-effort: if the
     * L2/driver does not support it, raw recv simply stays unavailable. */
    int prc = net_promisc_mode_on(data->iface);
    if (prc < 0 && prc != -EALREADY) {
        LOG_WRN("802.15.4 promiscuous mode unavailable (%d); raw recv disabled", prc);
    }
#endif
    
    data->initialized = true;
    handle->state = RADIO_STATE_IDLE;
    
    LOG_INF("802.15.4 radio initialized - EUI-64: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
            data->hw_addr[0], data->hw_addr[1], data->hw_addr[2], data->hw_addr[3],
            data->hw_addr[4], data->hw_addr[5], data->hw_addr[6], data->hw_addr[7]);
    
    return 0;
}

static int ieee802154_radio_deinit(radio_handle_t *handle)
{
    struct ieee802154_radio_data *data = handle->priv_data;
    
#if defined(CONFIG_AKIRA_RADIO_802154_PROMISC_RX)
    if (data->iface) {
        (void)net_promisc_mode_off(data->iface);
    }
#endif
    
    data->initialized = false;
    handle->state = RADIO_STATE_OFF;
    
    LOG_INF("802.15.4 radio deinitialized");
    return 0;
}

static int ieee802154_radio_configure(radio_handle_t *handle, const radio_config_t *config)
{
    struct ieee802154_radio_data *data = handle->priv_data;
    
    LOG_DBG("802.15.4 radio configuration (channel=%d, power=%d dBm)",
            config->channel, config->tx_power);
    
    /* Configure channel (11-26 for 2.4 GHz) */
    if (config->channel >= 11 && config->channel <= 26) {
        /* Channel configuration would use driver-specific API */
        LOG_DBG("Setting 802.15.4 channel to %d", config->channel);
    }
    
    /* Configure TX power */
    if (config->tx_power >= -40 && config->tx_power <= 20) {
        LOG_DBG("Setting TX power to %d dBm", config->tx_power);
    }
    
    return 0;
}

static int ieee802154_radio_get_config(radio_handle_t *handle, radio_config_t *config)
{
    struct ieee802154_radio_data *data = handle->priv_data;
    
    memset(config, 0, sizeof(*config));
    config->channel = 11;    /* Default 802.15.4 channel */
    config->mtu = 127;       /* 802.15.4 maximum frame size */
    config->tx_power = 0;    /* Default 0 dBm */
    config->retry_count = 3; /* Default retries */
    config->auto_ack = true; /* Auto-ACK enabled */
    
    return 0;
}

static int ieee802154_radio_send(radio_handle_t *handle, const uint8_t *data, size_t len)
{
    struct ieee802154_radio_data *radio_data = handle->priv_data;
    
    if (len > 127) {
        LOG_ERR("802.15.4 frame too large: %zu bytes (max 127)", len);
        return -EMSGSIZE;
    }
    
    /* 802.15.4 transmission requires proper frame construction */
    /* This is typically handled by the net stack, not raw radio */
    LOG_WRN("802.15.4 raw send not fully implemented - use net stack");
    
    radio_data->stats.tx_packets++;
    radio_data->stats.tx_bytes += len;
    
    return len;
}

static int ieee802154_radio_recv(radio_handle_t *handle, uint8_t *buf, size_t buf_len,
                                uint32_t timeout_ms)
{
#if defined(CONFIG_AKIRA_RADIO_802154_PROMISC_RX)
    struct ieee802154_radio_data *radio_data = handle->priv_data;
    struct net_pkt *pkt;
    size_t frame_len;
    int ret;
    
    if (!buf || buf_len == 0) {
        return -EINVAL;
    }
    
    /* Block on the promiscuous RX queue for the next raw 802.15.4 frame. The
     * net L2 feeds every received frame here once promiscuous mode is enabled
     * (see ieee802154_radio_init). timeout_ms == 0 polls without blocking. */
    pkt = net_promisc_mode_wait_data(timeout_ms == 0U ? K_NO_WAIT : K_MSEC(timeout_ms));
    if (!pkt) {
        return -EAGAIN;  /* timed out, no frame available */
    }
    
    /* The promiscuous queue is shared across all promiscuous interfaces; drop
     * frames that did not arrive on our 802.15.4 interface. */
    if (net_pkt_iface(pkt) != radio_data->iface) {
        net_pkt_unref(pkt);
        return -EAGAIN;
    }
    
    frame_len = net_pkt_get_len(pkt);
    if (frame_len > buf_len) {
        LOG_WRN("802.15.4 RX frame %zu B truncated to %zu B buffer", frame_len, buf_len);
        frame_len = buf_len;
    }
    
    net_pkt_cursor_init(pkt);
    ret = net_pkt_read(pkt, buf, frame_len);
    if (ret < 0) {
        LOG_ERR("802.15.4 RX frame copy failed: %d", ret);
        radio_data->stats.rx_errors++;
        net_pkt_unref(pkt);
        return ret;
    }
    
    /* Latch per-frame link metadata so get_stats() reports real values. */
    radio_data->last_lqi = net_pkt_ieee802154_lqi(pkt);
    radio_data->last_rssi = net_pkt_ieee802154_rssi_dbm(pkt);
    radio_data->have_rx_meta = true;
    
    radio_data->stats.rx_packets++;
    radio_data->stats.rx_bytes += frame_len;
    
    net_pkt_unref(pkt);
    return (int)frame_len;
#else
    ARG_UNUSED(handle);
    ARG_UNUSED(buf);
    ARG_UNUSED(buf_len);
    ARG_UNUSED(timeout_ms);
    /* Raw RX needs promiscuous-mode capture (CONFIG_NET_PROMISCUOUS_MODE).
     * Without it there is no in-tree path to pull raw frames from the L2. */
    LOG_WRN("802.15.4 raw recv unavailable: enable CONFIG_AKIRA_RADIO_802154_PROMISC_RX");
    return -ENOSYS;
#endif /* CONFIG_AKIRA_RADIO_802154_PROMISC_RX */
}

static int ieee802154_radio_scan(radio_handle_t *handle, uint32_t timeout_ms)
{
    struct ieee802154_radio_data *data = handle->priv_data;
    
    LOG_INF("Starting 802.15.4 scan (timeout=%u ms)", timeout_ms);
    
    /* Energy detection scan across all channels */
    handle->state = RADIO_STATE_SCAN;
    
    /* Scan implementation would iterate through channels 11-26 */
    for (uint8_t channel = 11; channel <= 26; channel++) {
        /* Perform energy detection on channel */
        LOG_DBG("Scanning channel %d", channel);
        k_msleep(timeout_ms / 16);  /* Divide time across channels */
    }
    
    handle->state = RADIO_STATE_IDLE;
    
    if (handle->event_cb) {
        radio_event_t event = {
            .type = RADIO_EVENT_SCAN_DONE,
            .user_data = handle->event_user_data,
        };
        handle->event_cb(&event, handle->event_user_data);
    }
    
    return 0;
}

static int ieee802154_radio_set_state(radio_handle_t *handle, radio_state_t state)
{
    LOG_DBG("802.15.4 radio set state: %s", radio_state_to_string(state));
    
    switch (state) {
    case RADIO_STATE_IDLE:
        /* Set radio to idle/standby */
        break;
    case RADIO_STATE_RX:
        /* Enable RX */
        break;
    case RADIO_STATE_SLEEP:
        /* Enter low power mode */
        break;
    default:
        break;
    }
    
    handle->state = state;
    return 0;
}

static radio_state_t ieee802154_radio_get_state(radio_handle_t *handle)
{
    return handle->state;
}

static int ieee802154_radio_get_stats(radio_handle_t *handle, radio_stats_t *stats)
{
    struct ieee802154_radio_data *data = handle->priv_data;
    memcpy(stats, &data->stats, sizeof(*stats));
    
    /*
     * LQI and RSSI are per-frame quantities in 802.15.4 (LQI range 0-255).
     * Report the values latched from the most recently received frame; if no
     * frame has been received yet (or its RSSI is undefined) report neutral /
     * unavailable sentinels instead of a fabricated constant.
     */
    if (data->have_rx_meta) {
        stats->lqi = data->last_lqi;
        if (data->last_rssi == IEEE802154_MAC_RSSI_DBM_UNDEFINED) {
            stats->rssi = RADIO_RSSI_UNAVAILABLE;
        } else {
            stats->rssi = (int8_t)CLAMP(data->last_rssi, INT8_MIN, INT8_MAX);
        }
    } else {
        stats->lqi = 0;
        stats->rssi = RADIO_RSSI_UNAVAILABLE;
    }
    
    return 0;
}

static int ieee802154_radio_reset(radio_handle_t *handle)
{
    struct ieee802154_radio_data *data = handle->priv_data;
    
    LOG_WRN("802.15.4 radio reset requested");
    
    /* Reset statistics */
    memset(&data->stats, 0, sizeof(data->stats));
    
    handle->state = RADIO_STATE_IDLE;
    return 0;
}

static int ieee802154_radio_set_event_callback(radio_handle_t *handle,
                                              radio_event_cb_t callback,
                                              void *user_data)
{
    handle->event_cb = callback;
    handle->event_user_data = user_data;
    LOG_DBG("802.15.4 event callback registered");
    return 0;
}

static int ieee802154_radio_get_hw_addr(radio_handle_t *handle, uint8_t *addr, size_t *addr_len)
{
    struct ieee802154_radio_data *data = handle->priv_data;
    
    if (!addr || !addr_len) {
        return -EINVAL;
    }
    
    size_t copy_len = MIN(*addr_len, 8);
    memcpy(addr, data->hw_addr, copy_len);
    *addr_len = copy_len;
    
    return 0;
}

/* 802.15.4 radio operations vtable */
static const radio_ops_t ieee802154_radio_ops = {
    .init = ieee802154_radio_init,
    .deinit = ieee802154_radio_deinit,
    .configure = ieee802154_radio_configure,
    .get_config = ieee802154_radio_get_config,
    .send = ieee802154_radio_send,
    .recv = ieee802154_radio_recv,
    .scan = ieee802154_radio_scan,
    .set_state = ieee802154_radio_set_state,
    .get_state = ieee802154_radio_get_state,
    .get_stats = ieee802154_radio_get_stats,
    .reset = ieee802154_radio_reset,
    .set_event_callback = ieee802154_radio_set_event_callback,
    .get_hw_addr = ieee802154_radio_get_hw_addr,
};

/* Initialize and register 802.15.4 radio */
int radio_802154_register(void)
{
    memset(&ieee802154_handle, 0, sizeof(ieee802154_handle));
    memset(&ieee802154_data, 0, sizeof(ieee802154_data));
    
    ieee802154_handle.type = RADIO_TYPE_802154;
    ieee802154_handle.name = "802.15.4";
    ieee802154_handle.capabilities = RADIO_CAP_TX | RADIO_CAP_RX | RADIO_CAP_SCAN |
                                    RADIO_CAP_MESH | RADIO_CAP_ENCRYPTION |
                                    RADIO_CAP_LOW_POWER | RADIO_CAP_CCA |
                                    RADIO_CAP_AUTO_ACK | RADIO_CAP_CSMA_CA |
                                    RADIO_CAP_RAW_MODE;
    ieee802154_handle.ops = &ieee802154_radio_ops;
    ieee802154_handle.priv_data = &ieee802154_data;
    ieee802154_handle.state = RADIO_STATE_OFF;
    
    /* Initialize the radio */
    int ret = ieee802154_radio_init(&ieee802154_handle);
    if (ret) {
        LOG_ERR("802.15.4 radio initialization failed: %d", ret);
        return ret;
    }
    
    /* Register with radio manager */
    ret = radio_manager_register(&ieee802154_handle);
    if (ret) {
        LOG_ERR("Failed to register 802.15.4 radio: %d", ret);
        return ret;
    }
    
    LOG_INF("802.15.4 radio registered successfully");
    return 0;
}

#endif /* CONFIG_NET_L2_IEEE802154 */
