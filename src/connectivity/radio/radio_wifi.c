/**
 * @file radio_wifi.c
 * @brief WiFi Radio Backend for Radio Abstraction Layer
 *
 * Implements RAL interface for IEEE 802.11 WiFi radios (ESP32, nRF7002, etc.)
 * Binds to Zephyr's WiFi management API. send()/recv() move raw AkiraMesh
 * frames over a UDP socket (mesh's own single-SoftAP segment, see
 * radio_wifi_set_gateway()) — unlike every other RAL backend, this one peeks
 * at the outgoing/incoming buffer's mesh_header (src_id/dest_id only) to
 * unicast to a known peer instead of always broadcasting, since WiFi is the
 * one medium where per-peer addressing changes throughput ~10x. That
 * constant layout is duplicated locally (WIFI_MESH_HDR_*) rather than
 * pulled in via mesh_routing.h, so this file stays buildable with
 * CONFIG_WIFI=y and CONFIG_AKIRA_MESH=n.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
*/

#include "connectivity/radio_interface.h"
#include "radio_wifi.h"
#include <zephyr/logging/log.h>
#include <zephyr/init.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/dhcpv4_server.h>
#include <string.h>
#include <errno.h>
#include "lib/mem_helper.h"

LOG_MODULE_REGISTER(radio_wifi, LOG_LEVEL_INF);

#ifdef CONFIG_WIFI

/* Mirrors struct mesh_header's leading fields (mesh_routing.h) byte-for-byte
 * — version(1) + msg_type(1) + ttl(1) + src_id[8] + dest_id[8]. Duplicated,
 * not included, so this generic RAL backend doesn't require CONFIG_AKIRA_MESH
 * to build. If mesh_header's layout ever changes, update both. */
#define WIFI_MESH_HDR_ID_LEN     8
#define WIFI_MESH_HDR_SRC_OFF    3
#define WIFI_MESH_HDR_DEST_OFF   (WIFI_MESH_HDR_SRC_OFF + WIFI_MESH_HDR_ID_LEN)
#define WIFI_MESH_HDR_MIN_LEN    (WIFI_MESH_HDR_DEST_OFF + WIFI_MESH_HDR_ID_LEN)

#define WIFI_MESH_PEER_MAX       8
#define WIFI_MESH_UDP_PORT       CONFIG_AKIRA_RADIO_WIFI_MESH_PORT
#define WIFI_MESH_AP_IP          "192.168.44.1"
#define WIFI_MESH_AP_NETMASK     "255.255.255.0"
#define WIFI_MESH_AP_DHCP_START  "192.168.44.10"

/* One learned peer: node_id -> IP, refreshed on every RX from that peer. */
struct wifi_mesh_peer {
    uint8_t node_id[WIFI_MESH_HDR_ID_LEN];
    struct in_addr ip;
    bool valid;
};

static bool wifi_mesh_id_is_broadcast(const uint8_t *id)
{
    for (int i = 0; i < WIFI_MESH_HDR_ID_LEN; i++) {
        if (id[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

/* WiFi radio private data */
struct wifi_radio_data {
    struct net_if *iface;
    struct net_mgmt_event_callback mgmt_cb;
    struct net_mgmt_event_callback ipv4_cb;
    struct k_sem scan_sem;
    struct k_sem connect_sem;
    radio_stats_t stats;
    int sock;                                    /* mesh UDP socket, -1 if unopened */
    struct wifi_mesh_peer peers[WIFI_MESH_PEER_MAX];
};

static struct wifi_radio_data wifi_data AKIRA_BULK_BSS;
static radio_handle_t wifi_handle;
static bool s_wifi_gateway;

void radio_wifi_set_gateway(bool is_gateway)
{
    s_wifi_gateway = is_gateway;
}

/* Learn/refresh a peer's IP from a received frame's src_id. No-op for a
 * broadcast src_id (never valid) or once the table is full — a full table
 * just means those peers' sends fall back to broadcast (see
 * wifi_mesh_peer_lookup()), not a hard failure. */
static void wifi_mesh_peer_learn(struct wifi_radio_data *data,
                                 const uint8_t *node_id, struct in_addr ip)
{
    if (wifi_mesh_id_is_broadcast(node_id)) {
        return;
    }
    int free_slot = -1;
    for (int i = 0; i < WIFI_MESH_PEER_MAX; i++) {
        if (data->peers[i].valid &&
            memcmp(data->peers[i].node_id, node_id, WIFI_MESH_HDR_ID_LEN) == 0) {
            data->peers[i].ip = ip;
            return;
        }
        if (free_slot < 0 && !data->peers[i].valid) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return;
    }
    memcpy(data->peers[free_slot].node_id, node_id, WIFI_MESH_HDR_ID_LEN);
    data->peers[free_slot].ip = ip;
    data->peers[free_slot].valid = true;
}

/* NULL if node_id is broadcast, unknown, or the table hasn't learned it yet
 * — caller falls back to a subnet broadcast send in that case. */
static const struct in_addr *wifi_mesh_peer_lookup(struct wifi_radio_data *data,
                                                    const uint8_t *node_id)
{
    if (wifi_mesh_id_is_broadcast(node_id)) {
        return NULL;
    }
    for (int i = 0; i < WIFI_MESH_PEER_MAX; i++) {
        if (data->peers[i].valid &&
            memcmp(data->peers[i].node_id, node_id, WIFI_MESH_HDR_ID_LEN) == 0) {
            return &data->peers[i].ip;
        }
    }
    return NULL;
}

/* IPv4 address event handler.
 * NET_EVENT_IPV4_ADDR_ADD fires when ESP-IDF's DHCP assigns an IP.
 * At this point ESP-IDF's DNS resolver has a server and getaddrinfo works. */
static void ipv4_mgmt_event_handler(struct net_mgmt_event_callback *cb,
                                    uint64_t mgmt_event, struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
        LOG_INF("IPv4 address assigned — DNS ready");
    }
}

/* WiFi management event handler */
static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
                                   uint64_t mgmt_event, struct net_if *iface)
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
        
    case NET_EVENT_WIFI_AP_ENABLE_RESULT:
        LOG_INF("mesh SoftAP enabled");
        k_sem_give(&data->connect_sem);
        break;

    case NET_EVENT_WIFI_CONNECT_RESULT:
        LOG_INF("WiFi connected");
        data->stats.rx_packets = 0;  /* Reset stats on reconnect */
        data->stats.tx_packets = 0;
        k_sem_give(&data->connect_sem);
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

/* Start the mesh SoftAP: static IP + Zephyr's own DHCPv4 server pool for
 * associating STAs — this WiFi driver doesn't bring up AP-mode DHCP on its
 * own. */
static int wifi_mesh_start_ap(struct net_if *iface, struct k_sem *ap_ready_sem)
{
    struct in_addr ap_addr, netmask, dhcp_start;

    if (net_addr_pton(AF_INET, WIFI_MESH_AP_IP, &ap_addr) < 0 ||
        net_addr_pton(AF_INET, WIFI_MESH_AP_NETMASK, &netmask) < 0 ||
        net_addr_pton(AF_INET, WIFI_MESH_AP_DHCP_START, &dhcp_start) < 0) {
        return -EINVAL;
    }

    net_if_ipv4_addr_add(iface, &ap_addr, NET_ADDR_MANUAL, 0);
    net_if_ipv4_set_netmask_by_addr(iface, &ap_addr, &netmask);

    int ret = net_dhcpv4_server_start(iface, &dhcp_start);
    if (ret && ret != -EALREADY) {
        LOG_ERR("mesh AP: DHCPv4 server start failed: %d", ret);
        return ret;
    }

    struct wifi_connect_req_params ap_params = {
        .ssid        = (const uint8_t *)CONFIG_AKIRA_RADIO_WIFI_MESH_SSID,
        .ssid_length = sizeof(CONFIG_AKIRA_RADIO_WIFI_MESH_SSID) - 1,
        .psk         = (const uint8_t *)CONFIG_AKIRA_RADIO_WIFI_MESH_PSK,
        .psk_length  = sizeof(CONFIG_AKIRA_RADIO_WIFI_MESH_PSK) - 1,
        .channel     = WIFI_CHANNEL_ANY,
        .band        = WIFI_FREQ_BAND_2_4_GHZ,
        .security    = (sizeof(CONFIG_AKIRA_RADIO_WIFI_MESH_PSK) - 1) > 0
                        ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
    };

    ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &ap_params, sizeof(ap_params));
    if (ret) {
        LOG_ERR("mesh AP enable failed: %d", ret);
        return ret;
    }

    /* AP_ENABLE is async — the interface isn't actually up yet when this
     * request returns 0, it just means the request was accepted. Sending on
     * it before NET_EVENT_WIFI_AP_ENABLE_RESULT fires fails at the driver
     * with -EIO. */
    ret = k_sem_take(ap_ready_sem, K_MSEC(CONFIG_AKIRA_RADIO_WIFI_MESH_CONNECT_TIMEOUT_MS));
    if (ret) {
        LOG_ERR("mesh SoftAP enable timed out");
        return -ETIMEDOUT;
    }

    LOG_INF("mesh SoftAP '%s' up at %s", CONFIG_AKIRA_RADIO_WIFI_MESH_SSID, WIFI_MESH_AP_IP);
    return 0;
}

/* STA-connect to the mesh SoftAP and block until associated (DHCP-assigned
 * IP arrives shortly after via CONFIG_ESP32_WIFI_STA_AUTO_DHCPV4). */
static int wifi_mesh_start_sta(struct net_if *iface, struct k_sem *connect_sem)
{
    struct wifi_connect_req_params params = {
        .ssid        = (const uint8_t *)CONFIG_AKIRA_RADIO_WIFI_MESH_SSID,
        .ssid_length = sizeof(CONFIG_AKIRA_RADIO_WIFI_MESH_SSID) - 1,
        .psk         = (const uint8_t *)CONFIG_AKIRA_RADIO_WIFI_MESH_PSK,
        .psk_length  = sizeof(CONFIG_AKIRA_RADIO_WIFI_MESH_PSK) - 1,
        .channel     = WIFI_CHANNEL_ANY,
        .band        = WIFI_FREQ_BAND_2_4_GHZ,
        .security    = (sizeof(CONFIG_AKIRA_RADIO_WIFI_MESH_PSK) - 1) > 0
                        ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
        .mfp         = WIFI_MFP_OPTIONAL,
        .timeout     = CONFIG_AKIRA_RADIO_WIFI_MESH_CONNECT_TIMEOUT_MS,
    };

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
    if (ret) {
        LOG_ERR("mesh STA connect request failed: %d", ret);
        return ret;
    }

    ret = k_sem_take(connect_sem, K_MSEC(CONFIG_AKIRA_RADIO_WIFI_MESH_CONNECT_TIMEOUT_MS));
    if (ret) {
        LOG_ERR("mesh STA connect to '%s' timed out", CONFIG_AKIRA_RADIO_WIFI_MESH_SSID);
        return -ETIMEDOUT;
    }

    LOG_INF("mesh STA connected to '%s'", CONFIG_AKIRA_RADIO_WIFI_MESH_SSID);
    return 0;
}

static int wifi_radio_init(radio_handle_t *handle)
{
    struct wifi_radio_data *data = handle->priv_data;

    /* ESP32's WiFi driver exposes separate SoftAP and STA net_if instances
     * (net_if_get_default() picks one arbitrarily, not necessarily the role
     * this init is bringing up) — must pick the matching one explicitly. */
    data->iface = s_wifi_gateway ? net_if_get_wifi_sap() : net_if_get_wifi_sta();
    if (!data->iface) {
        LOG_ERR("No %s WiFi interface found", s_wifi_gateway ? "SoftAP" : "STA");
        return -ENODEV;
    }

    /* Initialize semaphores for scan/connect operations */
    k_sem_init(&data->scan_sem, 0, 1);
    k_sem_init(&data->connect_sem, 0, 1);
    data->sock = -1;
    memset(data->peers, 0, sizeof(data->peers));

    /* Register for WiFi management events */
    net_mgmt_init_event_callback(&data->mgmt_cb, wifi_mgmt_event_handler,
                                NET_EVENT_WIFI_SCAN_DONE |
                                NET_EVENT_WIFI_AP_ENABLE_RESULT |
                                NET_EVENT_WIFI_CONNECT_RESULT |
                                NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&data->mgmt_cb);

    /* Register for IPv4 addr add — fires when DHCP assigns IP on ESP32 */
    net_mgmt_init_event_callback(&data->ipv4_cb, ipv4_mgmt_event_handler,
                                NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&data->ipv4_cb);

    int ret = s_wifi_gateway ? wifi_mesh_start_ap(data->iface, &data->connect_sem)
                             : wifi_mesh_start_sta(data->iface, &data->connect_sem);
    if (ret) {
        net_mgmt_del_event_callback(&data->mgmt_cb);
        net_mgmt_del_event_callback(&data->ipv4_cb);
        return ret;
    }

    data->sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (data->sock < 0) {
        LOG_ERR("mesh UDP socket() failed: %d", errno);
        return -errno;
    }

    int bcast_enable = 1;
    zsock_setsockopt(data->sock, SOL_SOCKET, SO_BROADCAST,
                     &bcast_enable, sizeof(bcast_enable));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(WIFI_MESH_UDP_PORT),
    };
    if (zsock_bind(data->sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        LOG_ERR("mesh UDP bind() failed: %d", errno);
        zsock_close(data->sock);
        data->sock = -1;
        return -errno;
    }

    LOG_INF("WiFi radio initialized (mesh %s, UDP port %d)",
           s_wifi_gateway ? "gateway/AP" : "node/STA", WIFI_MESH_UDP_PORT);
    handle->state = RADIO_STATE_IDLE;

    return 0;
}

static int wifi_radio_deinit(radio_handle_t *handle)
{
    struct wifi_radio_data *data = handle->priv_data;

    if (data->sock >= 0) {
        zsock_close(data->sock);
        data->sock = -1;
    }

    if (s_wifi_gateway) {
        net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, data->iface, NULL, 0);
    } else {
        net_mgmt(NET_REQUEST_WIFI_DISCONNECT, data->iface, NULL, 0);
    }

    /* Unregister management events */
    net_mgmt_del_event_callback(&data->mgmt_cb);
    net_mgmt_del_event_callback(&data->ipv4_cb);

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

    if (radio_data->sock < 0) {
        return -ENODEV;
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(WIFI_MESH_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    if (len >= WIFI_MESH_HDR_MIN_LEN) {
        const struct in_addr *peer_ip =
            wifi_mesh_peer_lookup(radio_data, &data[WIFI_MESH_HDR_DEST_OFF]);
        if (peer_ip) {
            dest.sin_addr = *peer_ip;
        }
    }

    int ret = zsock_sendto(radio_data->sock, data, len, 0,
                           (struct sockaddr *)&dest, sizeof(dest));
    if (ret < 0) {
        radio_data->stats.tx_errors++;
        return -errno;
    }

    radio_data->stats.tx_packets++;
    radio_data->stats.tx_bytes += len;
    return 0;
}

static int wifi_radio_recv(radio_handle_t *handle, uint8_t *buf, size_t buf_len,
                          uint32_t timeout_ms)
{
    struct wifi_radio_data *radio_data = handle->priv_data;

    if (radio_data->sock < 0) {
        return -ENODEV;
    }

    struct zsock_pollfd pfd = { .fd = radio_data->sock, .events = ZSOCK_POLLIN };
    int pret = zsock_poll(&pfd, 1, (int)timeout_ms);
    if (pret <= 0) {
        return 0; /* timeout or poll error: no frame ready, matches other backends' recv() contract */
    }

    struct sockaddr_in src = {0};
    socklen_t src_len = sizeof(src);
    int n = zsock_recvfrom(radio_data->sock, buf, buf_len, 0,
                           (struct sockaddr *)&src, &src_len);
    if (n < 0) {
        radio_data->stats.rx_errors++;
        return 0;
    }

    if ((size_t)n >= WIFI_MESH_HDR_MIN_LEN) {
        wifi_mesh_peer_learn(radio_data, &buf[WIFI_MESH_HDR_SRC_OFF], src.sin_addr);
    }

    radio_data->stats.rx_packets++;
    radio_data->stats.rx_bytes += n;
    return n;
}

static int wifi_radio_get_max_payload(radio_handle_t *handle, size_t *max_len)
{
    ARG_UNUSED(handle);
    *max_len = CONFIG_AKIRA_RADIO_WIFI_MESH_MTU;
    return 0;
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
    if (!link_addr) {
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
    .get_max_payload = wifi_radio_get_max_payload,
    .scan = wifi_radio_scan,
    .set_state = wifi_radio_set_state,
    .get_state = wifi_radio_get_state,
    .get_stats = wifi_radio_get_stats,
    .reset = wifi_radio_reset,
    .set_event_callback = wifi_radio_set_event_callback,
    .get_hw_addr = wifi_radio_get_hw_addr,
};

/* Registers the handle only; ops->init() (AP/STA bring-up, socket open) is
 * deferred to whoever acquires the radio, same as radio_ble.c — avoids
 * bringing WiFi up before a caller (mesh) has decided gateway vs. node role
 * via radio_wifi_set_gateway(). */
int radio_wifi_register(void)
{
    memset(&wifi_handle, 0, sizeof(wifi_handle));
    memset(&wifi_data, 0, sizeof(wifi_data));
    wifi_data.sock = -1;

    wifi_handle.type = RADIO_TYPE_WIFI;
    wifi_handle.name = "WiFi";
    wifi_handle.capabilities = RADIO_CAP_TX | RADIO_CAP_RX | RADIO_CAP_SCAN |
                              RADIO_CAP_MESH | RADIO_CAP_ENCRYPTION |
                              RADIO_CAP_MULTICAST | RADIO_CAP_RAW_MODE;
    wifi_handle.ops = &wifi_radio_ops;
    wifi_handle.priv_data = &wifi_data;
    wifi_handle.state = RADIO_STATE_OFF;

    int ret = radio_manager_register(&wifi_handle);
    if (ret) {
        LOG_ERR("Failed to register WiFi radio: %d", ret);
        return ret;
    }

    LOG_INF("WiFi radio registered successfully");
    return 0;
}

static int wifi_radio_auto_register(void)
{
    int ret = radio_wifi_register();

    if (ret < 0 && ret != -EALREADY) {
        LOG_ERR("Failed to auto-register WiFi radio: %d", ret);
        return ret;
    }
    return 0;
}

SYS_INIT(wifi_radio_auto_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_WIFI */
