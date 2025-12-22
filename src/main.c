/**
 * @file main.c
 * @brief AkiraOS Main Entry Point
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

/* Hardware */
#include "drivers/platform_hal.h"
#include "drivers/driver_registry.h"

/* Connectivity */
#ifdef CONFIG_WIFI
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#endif
#ifdef CONFIG_BT
#include "connectivity/bluetooth/bt_manager.h"
#endif
#ifdef CONFIG_USB_DEVICE_STACK
#include "connectivity/usb/usb_manager.h"
#endif

/* HID */
#ifdef CONFIG_AKIRA_HID
#include "connectivity/hid/hid_manager.h"
#endif
#ifdef CONFIG_AKIRA_HID_SIM
#include "connectivity/hid/hid_sim.h"
#endif
#ifdef CONFIG_AKIRA_BT_HID
#include "connectivity/bluetooth/bt_hid.h"
#endif

/* Storage & Settings */
#ifdef CONFIG_FILE_SYSTEM
#include "storage/fs_manager.h"
#endif
#ifdef CONFIG_AKIRA_SETTINGS
#include "settings/settings.h"
#endif

/* Services */
#ifdef CONFIG_AKIRA_APP_MANAGER
#include "services/app_manager.h"
#endif
#ifdef CONFIG_AKIRA_SHELL
#include "shell/akira_shell.h"
#endif
#ifdef CONFIG_AKIRA_HTTP_SERVER
#include "OTA/web_server.h"
#endif

/* OTA Manager */
#ifdef CONFIG_AKIRA_OTA
#include "OTA/ota_manager.h"
#endif

LOG_MODULE_REGISTER(akira_main, CONFIG_AKIRA_LOG_LEVEL);

int main(void)
{
    printk("\n════════════════════════════════════════\n");
    printk("          AkiraOS v1.3.0\n");
    printk("   Modular Embedded Operating System\n");
    printk("════════════════════════════════════════\n\n");
    LOG_INF("Build: %s %s", __DATE__, __TIME__);

    /* Hardware initialization */
    if (akira_hal_init() < 0)
    {
        LOG_ERR("HAL init failed");
        return -1;
    }

    if (driver_registry_init() < 0)
    {
        LOG_ERR("Driver registry failed");
        return -1;
    }

    /* Storage (optional) */
#ifdef CONFIG_FILE_SYSTEM
    if (fs_manager_init() < 0)
    {
        LOG_WRN("Storage init failed");
    }
#endif

    /* Settings (optional) */
#ifdef CONFIG_AKIRA_SETTINGS
    if (user_settings_init() < 0)
    {
        LOG_WRN("Settings init failed");
    }
#endif

    /* WiFi (optional) */
#ifdef CONFIG_WIFI
    struct net_if *iface = net_if_get_default();
    if (iface)
    {
        LOG_INF("WiFi interface ready");
    }
    else
    {
        LOG_WRN("No WiFi interface found");
    }
#endif

    /* Bluetooth (optional) */
#ifdef CONFIG_BT
    bt_config_t bt_cfg = {
        .device_name = "AkiraOS",
        .vendor_id = 0xFFFF,
        .product_id = 0x0001,
        .services = BT_SERVICE_ALL,
        .auto_advertise = true,
        .pairable = true};
    if (bt_manager_init(&bt_cfg) < 0)
    {
        LOG_WRN("Bluetooth init failed");
    }

    /* HID subsystem initialization */
#ifdef CONFIG_AKIRA_HID
    hid_config_t hid_cfg = {
        .device_types = HID_DEVICE_KEYBOARD | HID_DEVICE_GAMEPAD,
        .preferred_transport = HID_TRANSPORT_BLE,
        .device_name = "AkiraOS HID",
        .vendor_id = 0x1234,
        .product_id = 0x5678,
    };

    if (hid_manager_init(&hid_cfg) < 0)
    {
        LOG_WRN("HID manager init failed");
    }

#ifdef CONFIG_AKIRA_HID_SIM
    hid_sim_init();
#endif /* CONFIG_AKIRA_HID_SIM */
#endif /* CONFIG_AKIRA_HID */

#ifdef CONFIG_AKIRA_BT_HID
    bt_hid_init();
    /* Default to BLE transport and enable HID so device advertises */
    hid_manager_set_transport(HID_TRANSPORT_BLE);
    hid_manager_enable();
#endif
#endif /* CONFIG_BT */

    /* USB (optional) */
#ifdef CONFIG_USB_DEVICE_STACK
    usb_config_t usb_cfg = {
        .manufacturer = "AkiraOS",
        .product = "AkiraOS Device",
        .serial = "123456",
        .vendor_id = 0xFFFF,
        .product_id = 0x0001,
        .classes = USB_CLASS_ALL};
    if (usb_manager_init(&usb_cfg) < 0)
    {
        LOG_WRN("USB init failed");
    }
#endif

    /* OTA Manager - initialize before app manager and web server */
#ifdef CONFIG_AKIRA_OTA
    if (ota_manager_init() < 0)
    {
        LOG_ERR("OTA manager init failed");
    }
#endif

    /* App manager (optional) - includes runtime initialization */
#ifdef CONFIG_AKIRA_APP_MANAGER
    if (app_manager_init() < 0)
    {
        LOG_WRN("App manager failed");
    }
#endif

    /* Shell (optional) */
#ifdef CONFIG_AKIRA_SHELL
    if (akira_shell_init() < 0)
    {
        LOG_WRN("Shell init failed");
    }
#endif

    /* Web server (optional) */
#ifdef CONFIG_AKIRA_HTTP_SERVER
    if (web_server_start(NULL) < 0)
    {
        LOG_WRN("Web server init failed");
    }
#endif

    LOG_INF("✅ AkiraOS is ready");

    /* Main loop - just sleep */
    while (1)
    {
        k_sleep(K_SECONDS(10));
    }

    return 0;
}
