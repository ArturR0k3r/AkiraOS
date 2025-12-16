/**
 * @file wifi_manager.c
 * @brief WiFi Manager Implementation
 */

#include "wifi_manager.h"
#include "../../core/event_bus.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_WIFI
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_ip.h>
#endif

LOG_MODULE_REGISTER(wifi_manager, CONFIG_AKIRA_LOG_LEVEL);

#ifdef CONFIG_WIFI

static struct {
    bool initialized;
    wifi_status_t status;
    char ssid[33];
    char ip_addr[16];
    struct net_mgmt_event_callback wifi_cb;
    struct k_work_delayable ip_work;
} wifi_state = {0};

static void get_ip_work_handler(struct k_work *work)
{
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        LOG_ERR("No default network interface");
        return;
    }

    struct in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (addr) {
        net_addr_ntop(AF_INET, addr, wifi_state.ip_addr, sizeof(wifi_state.ip_addr));
        LOG_INF("WiFi IP: %s", wifi_state.ip_addr);
        
        /* Publish connected event with IP */
        system_event_t event = {
            .type = EVENT_NETWORK_CONNECTED,
            .timestamp = k_uptime_get(),
            .data.network = {
                .type = NETWORK_TYPE_WIFI,
                .connected = true
            }
        };
        strncpy(event.data.network.ip_addr, wifi_state.ip_addr, 
                sizeof(event.data.network.ip_addr) - 1);
        event_bus_publish(&event);
    }
}

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event, struct net_if *iface)
{
    switch (mgmt_event) {
        case NET_EVENT_WIFI_CONNECT_RESULT: {
            const struct wifi_status *status = (const struct wifi_status *)cb->info;
            if (status->status == 0) {
                LOG_INF("WiFi connected");
                wifi_state.status = WIFI_STATUS_CONNECTED;
                /* Schedule IP address retrieval */
                k_work_schedule(&wifi_state.ip_work, K_SECONDS(1));
            } else {
                LOG_ERR("WiFi connection failed: %d", status->status);
                wifi_state.status = WIFI_STATUS_FAILED;
                
                system_event_t event = {
                    .type = EVENT_NETWORK_ERROR,
                    .timestamp = k_uptime_get(),
                    .data.network.type = NETWORK_TYPE_WIFI
                };
                event_bus_publish(&event);
            }
            break;
        }
        
        case NET_EVENT_WIFI_DISCONNECT_RESULT:
            LOG_INF("WiFi disconnected");
            wifi_state.status = WIFI_STATUS_DISCONNECTED;
            wifi_state.ip_addr[0] = '\0';
            
            system_event_t event = {
                .type = EVENT_NETWORK_DISCONNECTED,
                .timestamp = k_uptime_get(),
                .data.network = {
                    .type = NETWORK_TYPE_WIFI,
                    .connected = false
                }
            };
            event_bus_publish(&event);
            break;
            
        default:
            break;
    }
}

int wifi_manager_init(void)
{
    if (wifi_state.initialized) {
        return 0;
    }
    
    LOG_INF("Initializing WiFi manager");
    
    wifi_state.status = WIFI_STATUS_DISCONNECTED;
    
    /* Initialize work queue for IP retrieval */
    k_work_init_delayable(&wifi_state.ip_work, get_ip_work_handler);
    
    /* Register network event callbacks */
    net_mgmt_init_event_callback(&wifi_state.wifi_cb, wifi_event_handler,
                                 NET_EVENT_WIFI_CONNECT_RESULT |
                                 NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_state.wifi_cb);
    
    wifi_state.initialized = true;
    
    LOG_INF("✅ WiFi manager initialized");
    return 0;
}

int wifi_manager_connect(const char *ssid, const char *password)
{
    if (!wifi_state.initialized) {
        return -ENODEV;
    }
    
    if (!ssid || !password) {
        return -EINVAL;
    }
    
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        return -ENODEV;
    }
    
    struct wifi_connect_req_params params = {0};
    
    strncpy(params.ssid, ssid, sizeof(params.ssid) - 1);
    params.ssid_length = strlen(params.ssid);
    
    params.psk = (uint8_t *)password;
    params.psk_length = strlen(password);
    
    params.channel = WIFI_CHANNEL_ANY;
    params.security = WIFI_SECURITY_TYPE_PSK;
    params.mfp = WIFI_MFP_OPTIONAL;
    params.timeout = SYS_FOREVER_MS;
    
    LOG_INF("Connecting to WiFi: %s", ssid);
    strncpy(wifi_state.ssid, ssid, sizeof(wifi_state.ssid) - 1);
    wifi_state.status = WIFI_STATUS_CONNECTING;
    
    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, 
                      &params, sizeof(params));
    if (ret < 0) {
        LOG_ERR("WiFi connect request failed: %d", ret);
        wifi_state.status = WIFI_STATUS_FAILED;
        return ret;
    }
    
    return 0;
}

int wifi_manager_disconnect(void)
{
    if (!wifi_state.initialized) {
        return -ENODEV;
    }
    
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        return -ENODEV;
    }
    
    LOG_INF("Disconnecting WiFi");
    
    int ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
    if (ret < 0) {
        LOG_ERR("WiFi disconnect request failed: %d", ret);
        return ret;
    }
    
    return 0;
}

wifi_status_t wifi_manager_get_status(void)
{
    return wifi_state.status;
}

int wifi_manager_get_ip(char *ip_addr, size_t len)
{
    if (!ip_addr || len == 0) {
        return -EINVAL;
    }
    
    if (wifi_state.ip_addr[0] == '\0') {
        return -ENODATA;
    }
    
    strncpy(ip_addr, wifi_state.ip_addr, len - 1);
    ip_addr[len - 1] = '\0';
    
    return 0;
}

#else /* !CONFIG_WIFI */

int wifi_manager_init(void)
{
    LOG_WRN("WiFi not configured");
    return -ENOTSUP;
}

int wifi_manager_connect(const char *ssid, const char *password)
{
    return -ENOTSUP;
}

int wifi_manager_disconnect(void)
{
    return -ENOTSUP;
}

wifi_status_t wifi_manager_get_status(void)
{
    return WIFI_STATUS_DISCONNECTED;
}

int wifi_manager_get_ip(char *ip_addr, size_t len)
{
    return -ENOTSUP;
}

#endif /* CONFIG_WIFI */
