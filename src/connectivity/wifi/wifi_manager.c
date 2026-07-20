/**
 * @file wifi_manager.c
 * @brief Centralized WiFi connection manager
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 */

#include "wifi_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_stats.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "../../settings/settings.h"

LOG_MODULE_REGISTER(wifi_manager, CONFIG_AKIRA_LOG_LEVEL);

/* ── internal state ─────────────────────────────────────────────────────── */

#define WIFI_MGR_MAX_SCAN_RESULTS 16

struct wifi_mgr_listener {
    wifi_mgr_event_cb_t cb;
    void               *user_data;
};

static struct {
    wifi_mgr_state_t    state;
    struct wifi_mgr_listener listeners[CONFIG_AKIRA_WIFI_MANAGER_MAX_CBS];
    int64_t             connect_time_ms;   /* k_uptime_get() at IP assignment */
    struct net_mgmt_event_callback wifi_cb;
    struct net_mgmt_event_callback ipv4_cb;
    struct k_mutex      lock;
    wifi_mgr_scan_result_t scan_results[WIFI_MGR_MAX_SCAN_RESULTS];
    size_t              scan_count;
    bool                scanning;
} mgr;

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void fire_event(wifi_mgr_event_t evt)
{
    for (int i = 0; i < CONFIG_AKIRA_WIFI_MANAGER_MAX_CBS; i++) {
        if (mgr.listeners[i].cb) {
            mgr.listeners[i].cb(evt, mgr.listeners[i].user_data);
        }
    }
}

/* ── net_mgmt callbacks ──────────────────────────────────────────────────── */

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                                uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    k_mutex_lock(&mgr.lock, K_FOREVER);

    switch (event) {
    case NET_EVENT_WIFI_CONNECT_RESULT: {
        const struct wifi_status *status =
            (const struct wifi_status *)cb->info;
        if (status && status->conn_status != WIFI_STATUS_CONN_SUCCESS) {
            LOG_WRN("WiFi connect failed (status=%d)", status->conn_status);
            mgr.state = WIFI_MGR_STATE_IDLE;
            k_mutex_unlock(&mgr.lock);
            fire_event(WIFI_MGR_EVT_CONNECT_FAILED);
            return;
        }
        /* L2 up — wait for IP assignment before declaring CONNECTED */
        LOG_DBG("WiFi L2 connected, waiting for IP");
        break;
    }

    case NET_EVENT_WIFI_DISCONNECT_RESULT:
        if (mgr.state == WIFI_MGR_STATE_CONNECTED ||
            mgr.state == WIFI_MGR_STATE_DISCONNECTING) {
            LOG_INF("WiFi disconnected");
            mgr.state = WIFI_MGR_STATE_IDLE;
            mgr.connect_time_ms = 0;
            k_mutex_unlock(&mgr.lock);
            fire_event(WIFI_MGR_EVT_DISCONNECTED);
            return;
        }
        break;

    /* AP-mode diagnostics — trace whether a client ever reaches L2
     * association, independent of whether it later gets a DHCP lease. */
    case NET_EVENT_WIFI_AP_ENABLE_RESULT: {
        const struct wifi_status *status = (const struct wifi_status *)cb->info;
        LOG_INF("WiFi AP enable result: status=%d", status ? status->status : -1);
        break;
    }
    case NET_EVENT_WIFI_AP_DISABLE_RESULT:
        LOG_INF("WiFi AP disabled");
        break;
    case NET_EVENT_WIFI_AP_STA_CONNECTED: {
        const struct wifi_ap_sta_info *sta = (const struct wifi_ap_sta_info *)cb->info;
        if (sta && sta->mac_length == 6) {
            LOG_INF("AP: station associated %02X:%02X:%02X:%02X:%02X:%02X",
                    sta->mac[0], sta->mac[1], sta->mac[2],
                    sta->mac[3], sta->mac[4], sta->mac[5]);
        } else {
            LOG_INF("AP: station associated (no MAC info)");
        }
        break;
    }
    case NET_EVENT_WIFI_AP_STA_DISCONNECTED:
        LOG_INF("AP: station disassociated");
        break;

    case NET_EVENT_WIFI_SCAN_RESULT: {
        if (!mgr.scanning || mgr.scan_count >= WIFI_MGR_MAX_SCAN_RESULTS) {
            break;
        }
        const struct wifi_scan_result *entry =
            (const struct wifi_scan_result *)cb->info;
        wifi_mgr_scan_result_t *dst = &mgr.scan_results[mgr.scan_count++];
        size_t len = MIN((size_t)entry->ssid_length, sizeof(dst->ssid) - 1);
        memcpy(dst->ssid, entry->ssid, len);
        dst->ssid[len] = '\0';
        dst->rssi     = entry->rssi;
        dst->security = (uint8_t)entry->security;
        dst->channel  = entry->channel;
        break;
    }

    case NET_EVENT_WIFI_SCAN_DONE:
        mgr.scanning = false;
        k_mutex_unlock(&mgr.lock);
        fire_event(WIFI_MGR_EVT_SCAN_DONE);
        return;

    default:
        break;
    }

    k_mutex_unlock(&mgr.lock);
}

/* DHCP's own dns_resolve_reconfigure() call (subsys/net/lib/dns/dhcpv4.c)
 * replaces the resolver's entire server list with just the DHCP-provided
 * unicast server, dropping any mDNS entry — every DHCP bind/renew. Re-add
 * the mDNS multicast address here, merged with the server DHCP just set,
 * so .local lookups (e.g. mqtt_service's broker) keep working. */
static void add_mdns_dns_server(void)
{
    struct dns_resolve_context *ctx = dns_resolve_get_default();
    const struct net_sockaddr *servers_sa[2] = { NULL, NULL };
    const char *servers_str[2] = { "224.0.0.251:5353", NULL };

    for (int i = 0; i < DNS_RESOLVER_MAX_POLL; i++) {
        if (ctx->servers[i].dns_server.sa_family != 0) {
            servers_sa[0] = &ctx->servers[i].dns_server;
            break;
        }
    }

    int ret = dns_resolve_reconfigure(ctx, servers_str, servers_sa, DNS_SOURCE_MANUAL);
    if (ret < 0) {
        LOG_WRN("Failed to re-add mDNS DNS server: %d", ret);
    }
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
                                uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    if (event != NET_EVENT_IPV4_ADDR_ADD) {
        return;
    }

    k_mutex_lock(&mgr.lock, K_FOREVER);

    if (mgr.state == WIFI_MGR_STATE_CONNECTING) {
        mgr.state = WIFI_MGR_STATE_CONNECTED;
        mgr.connect_time_ms = k_uptime_get();
        LOG_INF("WiFi connected with IP");
        k_mutex_unlock(&mgr.lock);
        add_mdns_dns_server();
        fire_event(WIFI_MGR_EVT_CONNECTED);
        return;
    }

    k_mutex_unlock(&mgr.lock);
}

/* ── public API ──────────────────────────────────────────────────────────── */

int wifi_manager_connect(void)
{
    k_mutex_lock(&mgr.lock, K_FOREVER);

    if (mgr.state == WIFI_MGR_STATE_CONNECTED ||
        mgr.state == WIFI_MGR_STATE_CONNECTING) {
        k_mutex_unlock(&mgr.lock);
        return -EALREADY;
    }

    struct net_if *iface = net_if_get_wifi_sta();
    if (!iface) {
        k_mutex_unlock(&mgr.lock);
        LOG_ERR("No STA network interface");
        return -ENODEV;
    }

    char ssid[MAX_VALUE_LEN];
    char psk[MAX_VALUE_LEN];

    /* SSID+PSK are stored as one tab-delimited value so the single NVS write
     * is atomic — separate writes leave a window where a power loss between
     * them strands a new SSID paired with the old PSK (or vice versa),
     * silently breaking connect on next boot until re-provisioned. */
    char combined[MAX_VALUE_LEN * 2 + 2];
    if (akira_settings_get(AKIRA_SETTINGS_WIFI_CREDS_KEY, combined, sizeof(combined)) != 0) {
        k_mutex_unlock(&mgr.lock);
        LOG_ERR("No WiFi credentials in NVS");
        return -ENOENT;
    }

    char *tab = strchr(combined, '\t');
    if (!tab) {
        k_mutex_unlock(&mgr.lock);
        LOG_ERR("Malformed combined WiFi credentials");
        return -EINVAL;
    }
    *tab = '\0';
    strncpy(ssid, combined, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    strncpy(psk, tab + 1, sizeof(psk) - 1);
    psk[sizeof(psk) - 1] = '\0';

    struct wifi_connect_req_params params = {
        .ssid        = (uint8_t *)ssid,
        .ssid_length = strlen(ssid),
        .psk         = (uint8_t *)psk,
        .psk_length  = strlen(psk),
        .channel     = WIFI_CHANNEL_ANY,
        .security    = strlen(psk) > 0 ? WIFI_SECURITY_TYPE_PSK
                                        : WIFI_SECURITY_TYPE_NONE,
        .mfp         = WIFI_MFP_OPTIONAL,
    };

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
    if (ret) {
        k_mutex_unlock(&mgr.lock);
        LOG_ERR("WiFi connect request failed: %d", ret);
        return ret;
    }

    mgr.state = WIFI_MGR_STATE_CONNECTING;
    LOG_INF("WiFi connecting to '%s'", ssid);

    k_mutex_unlock(&mgr.lock);
    return 0;
}

int wifi_manager_disconnect(void)
{
    k_mutex_lock(&mgr.lock, K_FOREVER);

    if (mgr.state == WIFI_MGR_STATE_IDLE ||
        mgr.state == WIFI_MGR_STATE_DISCONNECTING) {
        k_mutex_unlock(&mgr.lock);
        return -EALREADY;
    }

    struct net_if *iface = net_if_get_wifi_sta();
    if (!iface) {
        k_mutex_unlock(&mgr.lock);
        return -ENODEV;
    }

    int ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
    if (ret) {
        k_mutex_unlock(&mgr.lock);
        LOG_ERR("WiFi disconnect request failed: %d", ret);
        return ret;
    }

    mgr.state = WIFI_MGR_STATE_DISCONNECTING;
    k_mutex_unlock(&mgr.lock);
    return 0;
}

int wifi_manager_update_credentials(const char *ssid, const char *psk)
{
    if (!ssid || !psk) {
        return -EINVAL;
    }

    /* Write SSID and PSK as one tab-delimited value so the single NVS write is
     * atomic — a power-loss between two separate writes can leave stale PSK or
     * SSID and cause a permanent connection failure on next boot. */
    char combined[MAX_VALUE_LEN * 2 + 2];
    int n = snprintf(combined, sizeof(combined), "%s\t%s", ssid, psk);
    if (n < 0 || (size_t)n >= sizeof(combined)) {
        return -ENAMETOOLONG;
    }

    int ret = akira_settings_set(AKIRA_SETTINGS_WIFI_CREDS_KEY, combined, true);
    if (ret) {
        LOG_ERR("Failed to save WiFi credentials: %d", ret);
        return ret;
    }

    LOG_INF("WiFi credentials updated (ssid='%s')", ssid);
    return 0;
}

int wifi_manager_scan(void)
{
    k_mutex_lock(&mgr.lock, K_FOREVER);

    if (mgr.scanning) {
        k_mutex_unlock(&mgr.lock);
        return -EBUSY;
    }

    struct net_if *iface = net_if_get_wifi_sta();
    if (!iface) {
        k_mutex_unlock(&mgr.lock);
        LOG_ERR("No STA network interface for WiFi scan");
        return -ENODEV;
    }

    mgr.scan_count = 0;

    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0);
    if (ret) {
        k_mutex_unlock(&mgr.lock);
        LOG_ERR("WiFi scan request failed: %d", ret);
        return ret;
    }

    mgr.scanning = true;
    k_mutex_unlock(&mgr.lock);
    return 0;
}

int wifi_manager_get_scan_results(wifi_mgr_scan_result_t *out, size_t max, size_t *count_out)
{
    if (!out || !count_out) {
        return -EINVAL;
    }

    k_mutex_lock(&mgr.lock, K_FOREVER);

    size_t n = MIN(max, mgr.scan_count);
    memcpy(out, mgr.scan_results, n * sizeof(wifi_mgr_scan_result_t));
    *count_out = n;

    k_mutex_unlock(&mgr.lock);
    return 0;
}

int wifi_manager_get_stats(wifi_mgr_stats_t *out)
{
    if (!out) {
        return -EINVAL;
    }

    k_mutex_lock(&mgr.lock, K_FOREVER);

    if (mgr.state != WIFI_MGR_STATE_CONNECTED) {
        k_mutex_unlock(&mgr.lock);
        return -ENOTCONN;
    }

    memset(out, 0, sizeof(*out));

    struct net_if *iface = net_if_get_wifi_sta();
    if (!iface) {
        k_mutex_unlock(&mgr.lock);
        return -ENODEV;
    }

    /* RSSI, channel, SSID from driver status */
    struct wifi_iface_status status;
    if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface,
                 &status, sizeof(status)) == 0) {
        out->rssi    = status.rssi;
        out->channel = status.channel;
        strncpy(out->ssid, (const char *)status.ssid,
                MIN(status.ssid_len, sizeof(out->ssid) - 1));
    }

    /* IP address — use the foreach API; direct struct access removed in Zephyr 4.x */
    struct in_addr *global = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (global) {
        net_addr_ntop(AF_INET, global, out->ip_addr, sizeof(out->ip_addr));
    }

    /* Time connected */
    out->time_connected_s = (uint32_t)((k_uptime_get() - mgr.connect_time_ms) / 1000);

    /* TX/RX bytes from net_if stats */
#if defined(CONFIG_NET_STATISTICS_ETHERNET)
    struct net_stats_eth stats_buf;
    if (net_mgmt(NET_REQUEST_STATS_GET_ETHERNET, iface,
                 &stats_buf, sizeof(stats_buf)) == 0) {
        out->tx_bytes = (uint32_t)stats_buf.bytes.sent;
        out->rx_bytes = (uint32_t)stats_buf.bytes.received;
    }
#endif

    k_mutex_unlock(&mgr.lock);
    return 0;
}

wifi_mgr_state_t wifi_manager_get_state(void)
{
    return mgr.state;
}

int wifi_manager_ap_start(const char *ssid, const char *psk)
{
    if (!ssid || strlen(ssid) == 0) {
        return -EINVAL;
    }

    struct net_if *iface = net_if_get_wifi_sap();
    if (!iface) {
        LOG_ERR("No AP network interface");
        return -ENODEV;
    }

    struct wifi_connect_req_params params = {
        .ssid        = (uint8_t *)ssid,
        .ssid_length = strlen(ssid),
        .psk         = (uint8_t *)psk,
        .psk_length  = psk ? strlen(psk) : 0,
        .channel     = WIFI_CHANNEL_ANY,
        .security    = (psk && strlen(psk) > 0) ? WIFI_SECURITY_TYPE_PSK
                                                  : WIFI_SECURITY_TYPE_NONE,
        .mfp         = WIFI_MFP_OPTIONAL,
    };

    int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &params, sizeof(params));
    if (ret) {
        LOG_ERR("WiFi AP enable request failed: %d", ret);
        return ret;
    }

    /* The esp32 Zephyr driver's ap_enable only brings up the radio — unlike
     * its STA path, it does not assign the interface an IP or run a DHCP
     * server. Without this, clients associate at L2 but never get a lease
     * and sit stuck "obtaining IP address" forever. */
    struct in_addr ap_addr, ap_netmask, pool_start;

    net_addr_pton(AF_INET, "192.168.4.1", &ap_addr);
    net_addr_pton(AF_INET, "255.255.255.0", &ap_netmask);
    net_addr_pton(AF_INET, "192.168.4.2", &pool_start);

    net_if_ipv4_addr_add(iface, &ap_addr, NET_ADDR_MANUAL, 0);
    net_if_ipv4_set_netmask_by_addr(iface, &ap_addr, &ap_netmask);
    /* AP is its own gateway. Without this the iface's gw stays 0.0.0.0, so
     * the DHCPv4 server's router option advertises "no gateway" — clients
     * (notably iOS) read that as a purely local/isolated network with no
     * possible route out and skip the captive-portal probe entirely, never
     * even attempting a DNS query against us. */
    net_if_ipv4_set_gw(iface, &ap_addr);

    ret = net_dhcpv4_server_start(iface, &pool_start);
    if (ret && ret != -EALREADY) {
        LOG_WRN("DHCPv4 server start failed: %d", ret);
    }

    LOG_INF("WiFi AP '%s' started (192.168.4.1)", ssid);
    return 0;
}

int wifi_manager_ap_stop(void)
{
    struct net_if *iface = net_if_get_wifi_sap();
    if (!iface) {
        return -ENODEV;
    }

    net_dhcpv4_server_stop(iface);

    int ret = net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, iface, NULL, 0);
    if (ret) {
        LOG_ERR("WiFi AP disable request failed: %d", ret);
        return ret;
    }

    LOG_INF("WiFi AP stopped");
    return 0;
}

int wifi_manager_register_cb(wifi_mgr_event_cb_t cb, void *user_data)
{
    if (!cb) {
        return -EINVAL;
    }

    k_mutex_lock(&mgr.lock, K_FOREVER);

    int free_slot = -1;
    for (int i = 0; i < CONFIG_AKIRA_WIFI_MANAGER_MAX_CBS; i++) {
        if (mgr.listeners[i].cb == cb &&
            mgr.listeners[i].user_data == user_data) {
            k_mutex_unlock(&mgr.lock);
            return -EALREADY;
        }
        if (free_slot < 0 && mgr.listeners[i].cb == NULL) {
            free_slot = i;
        }
    }

    if (free_slot < 0) {
        k_mutex_unlock(&mgr.lock);
        LOG_WRN("Callback table full (max %d)", CONFIG_AKIRA_WIFI_MANAGER_MAX_CBS);
        return -ENOMEM;
    }

    mgr.listeners[free_slot].cb        = cb;
    mgr.listeners[free_slot].user_data = user_data;

    k_mutex_unlock(&mgr.lock);
    return 0;
}

int wifi_manager_unregister_cb(wifi_mgr_event_cb_t cb, void *user_data)
{
    if (!cb) {
        return -EINVAL;
    }

    k_mutex_lock(&mgr.lock, K_FOREVER);

    for (int i = 0; i < CONFIG_AKIRA_WIFI_MANAGER_MAX_CBS; i++) {
        if (mgr.listeners[i].cb == cb &&
            mgr.listeners[i].user_data == user_data) {
            mgr.listeners[i].cb        = NULL;
            mgr.listeners[i].user_data = NULL;
            k_mutex_unlock(&mgr.lock);
            return 0;
        }
    }

    k_mutex_unlock(&mgr.lock);
    return -ENOENT;
}

/* ── SYS_INIT ────────────────────────────────────────────────────────────── */

static int wifi_manager_init(void)
{
    memset(&mgr, 0, sizeof(mgr));
    k_mutex_init(&mgr.lock);
    mgr.state = WIFI_MGR_STATE_IDLE;

    net_mgmt_init_event_callback(&mgr.wifi_cb, wifi_event_handler,
                                 NET_EVENT_WIFI_CONNECT_RESULT |
                                 NET_EVENT_WIFI_DISCONNECT_RESULT |
                                 NET_EVENT_WIFI_AP_ENABLE_RESULT |
                                 NET_EVENT_WIFI_AP_DISABLE_RESULT |
                                 NET_EVENT_WIFI_AP_STA_CONNECTED |
                                 NET_EVENT_WIFI_AP_STA_DISCONNECTED |
                                 NET_EVENT_WIFI_SCAN_RESULT |
                                 NET_EVENT_WIFI_SCAN_DONE);
    net_mgmt_add_event_callback(&mgr.wifi_cb);

    net_mgmt_init_event_callback(&mgr.ipv4_cb, ipv4_event_handler,
                                 NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&mgr.ipv4_cb);

    LOG_INF("WiFi manager initialized");
    return 0;
}

SYS_INIT(wifi_manager_init, APPLICATION, CONFIG_AKIRA_WIFI_MANAGER_INIT_PRIORITY);
