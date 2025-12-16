/**
 * @file network_manager.c
 * @brief Network Subsystem Manager Implementation
 */

#include "network_manager.h"
#include "event_bus.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>

#ifdef CONFIG_WIFI
#include <zephyr/net/wifi_mgmt.h>
#include "../connectivity/wifi/wifi_manager.h"
#endif

#ifdef CONFIG_BT
#include "../connectivity/bluetooth/bt_manager.h"
#endif

#ifdef CONFIG_USB_DEVICE_STACK
#include "../connectivity/usb/usb_manager.h"
#endif

#ifdef CONFIG_AKIRA_CLOUD_CLIENT
#include "../connectivity/cloud/cloud_client.h"
#endif

LOG_MODULE_REGISTER(net_manager, CONFIG_AKIRA_LOG_LEVEL);

static struct {
    bool initialized;
    bool wifi_enabled;
    bool bt_enabled;
    bool usb_enabled;
    bool any_connected;
} net_state = {0};

#ifdef CONFIG_WIFI
static void wifi_event_handler(const system_event_t *event, void *user_data)
{
    switch (event->type) {
        case EVENT_NETWORK_CONNECTED:
            if (event->data.network.type == NETWORK_TYPE_WIFI) {
                LOG_INF("WiFi connected: %s", event->data.network.ip_addr);
                net_state.any_connected = true;
            }
            break;
            
        case EVENT_NETWORK_DISCONNECTED:
            if (event->data.network.type == NETWORK_TYPE_WIFI) {
                LOG_INF("WiFi disconnected");
                /* Check if any other interface is still connected */
                net_state.any_connected = net_state.bt_enabled || net_state.usb_enabled;
            }
            break;
            
        default:
            break;
    }
}

static int init_wifi(void)
{
    int ret;
    
    LOG_INF("Initializing WiFi");
    
    /* Subscribe to WiFi events */
    event_bus_subscribe(EVENT_NETWORK_CONNECTED, wifi_event_handler, NULL);
    event_bus_subscribe(EVENT_NETWORK_DISCONNECTED, wifi_event_handler, NULL);
    
    ret = wifi_manager_init();
    if (ret < 0) {
        LOG_ERR("WiFi manager initialization failed: %d", ret);
        return ret;
    }
    
    net_state.wifi_enabled = true;
    LOG_INF("✅ WiFi initialized");
    
    return 0;
}
#else
static int init_wifi(void)
{
    LOG_DBG("WiFi not configured");
    return 0;
}
#endif /* CONFIG_WIFI */

#ifdef CONFIG_BT
static void bt_event_handler(const system_event_t *event, void *user_data)
{
    switch (event->type) {
        case EVENT_BT_CONNECTED:
            LOG_INF("Bluetooth connected");
            net_state.any_connected = true;
            break;
            
        case EVENT_BT_DISCONNECTED:
            LOG_INF("Bluetooth disconnected");
            net_state.any_connected = net_state.wifi_enabled || net_state.usb_enabled;
            break;
            
        default:
            break;
    }
}

static int init_bluetooth(void)
{
    int ret;
    
    LOG_INF("Initializing Bluetooth");
    
    /* Subscribe to BT events */
    event_bus_subscribe(EVENT_BT_CONNECTED, bt_event_handler, NULL);
    event_bus_subscribe(EVENT_BT_DISCONNECTED, bt_event_handler, NULL);
    
    ret = bt_manager_init();
    if (ret < 0) {
        LOG_ERR("Bluetooth manager initialization failed: %d", ret);
        return ret;
    }
    
    net_state.bt_enabled = true;
    LOG_INF("✅ Bluetooth initialized");
    
    return 0;
}
#else
static int init_bluetooth(void)
{
    LOG_DBG("Bluetooth not configured");
    return 0;
}
#endif /* CONFIG_BT */

#ifdef CONFIG_USB_DEVICE_STACK
static int init_usb(void)
{
    int ret;
    
    LOG_INF("Initializing USB");
    
    ret = usb_manager_init();
    if (ret < 0) {
        LOG_ERR("USB manager initialization failed: %d", ret);
        return ret;
    }
    
    net_state.usb_enabled = true;
    LOG_INF("✅ USB initialized");
    
    return 0;
}
#else
static int init_usb(void)
{
    LOG_DBG("USB not configured");
    return 0;
}
#endif /* CONFIG_USB_DEVICE_STACK */

int network_manager_init(void)
{
    int ret;
    
    if (net_state.initialized) {
        return 0;
    }
    
    LOG_INF("Initializing network manager");
    
    /* Initialize WiFi */
    ret = init_wifi();
    if (ret < 0) {
        LOG_ERR("WiFi initialization failed: %d", ret);
        /* Continue with other interfaces */
    }
    
    /* Initialize Bluetooth */
    ret = init_bluetooth();
    if (ret < 0) {
        LOG_ERR("Bluetooth initialization failed: %d", ret);
        /* Continue with other interfaces */
    }
    
    /* Initialize USB */
    ret = init_usb();
    if (ret < 0) {
        LOG_ERR("USB initialization failed: %d", ret);
        /* Continue */
    }
    
#ifdef CONFIG_AKIRA_CLOUD_CLIENT
    /* Initialize cloud client if network is available */
    if (net_state.wifi_enabled) {
        ret = cloud_client_init();
        if (ret < 0) {
            LOG_WRN("Cloud client initialization failed: %d", ret);
        } else {
            LOG_INF("✅ Cloud client initialized");
        }
    }
#endif
    
    net_state.initialized = true;
    LOG_INF("✅ Network manager ready");
    
    return 0;
}

bool network_manager_is_ready(void)
{
    return net_state.initialized;
}

bool network_manager_is_connected(void)
{
    return net_state.any_connected;
}
