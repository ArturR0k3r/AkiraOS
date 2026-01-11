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
#if defined(CONFIG_NETWORKING)
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#endif
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
    /* Early banner - should print immediately */
    printk("\n════════════════════════════════════════\n");
    printk("          AkiraOS v1.3.3\n");
    printk("   Modular Embedded Operating System\n");
    printk("════════════════════════════════════════\n");
    LOG_INF("Build: %s %s", __DATE__, __TIME__);
    LOG_INF("Starting initialization...\n");

    /* Hardware initialization */
    LOG_INF("Initializing HAL...");
    if (akira_hal_init() < 0)
    {
        LOG_ERR("❌ HAL init failed - check platform_hal.c");
        /* Don't return, try to continue */
    } else {
        LOG_INF("✅ HAL initialized");
    }

    LOG_INF("Initializing driver registry...");
    if (driver_registry_init() < 0)
    {
        LOG_ERR("❌ Driver registry failed - check driver_registry.c");
        /* Don't return, try to continue */
    } else {
        LOG_INF("✅ Driver registry initialized");
    }

    /* Storage (optional) */
#ifdef CONFIG_FILE_SYSTEM
    LOG_INF("Initializing storage...");
    if (fs_manager_init() < 0)
    {
        LOG_WRN("⚠️ Storage init failed");
    } else {
        LOG_INF("✅ Storage initialized");
    }
#endif

    /* Settings (optional) */
#ifdef CONFIG_AKIRA_SETTINGS
    LOG_INF("Initializing settings...");
    if (user_settings_init() < 0)
    {
        LOG_WRN("⚠️ Settings init failed");
    } else {
        LOG_INF("✅ Settings initialized");
    }
#endif

    /* WiFi (optional) */
#if defined(CONFIG_WIFI) && defined(CONFIG_NETWORKING)
    LOG_INF("Checking WiFi interface...");
    struct net_if *iface = net_if_get_default();
    if (iface)
    {
        LOG_INF("✅ WiFi interface ready");
    }
    else
    {
        LOG_WRN("⚠️ No WiFi interface found");
    }
#endif

    /* Bluetooth (optional) */
#ifdef CONFIG_BT
    LOG_INF("Initializing Bluetooth...");
    bt_config_t bt_cfg = {
        .device_name = "AkiraOS",
        .vendor_id = 0xFFFF,
        .product_id = 0x0001,
        .services = BT_SERVICE_ALL,
        .auto_advertise = true,
        .pairable = true};
    if (bt_manager_init(&bt_cfg) < 0)
    {
        LOG_WRN("⚠️ Bluetooth init failed");
    } else {
        LOG_INF("✅ Bluetooth initialized");
    }

    /* HID subsystem initialization */
#ifdef CONFIG_AKIRA_HID
    LOG_INF("Initializing HID...");
    hid_config_t hid_cfg = {
        .device_types = HID_DEVICE_KEYBOARD | HID_DEVICE_GAMEPAD,
        .preferred_transport = HID_TRANSPORT_BLE,
        .device_name = "AkiraOS HID",
        .vendor_id = 0x1234,
        .product_id = 0x5678,
    };

    if (hid_manager_init(&hid_cfg) < 0)
    {
        LOG_WRN("⚠️ HID manager init failed");
    } else {
        LOG_INF("✅ HID initialized");
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
        LOG_WRN("⚠️ USB init failed");
    } else {
        LOG_INF("✅ USB initialized");
    }
#endif

    /* OTA Manager - initialize before app manager and web server */
#ifdef CONFIG_AKIRA_OTA
    LOG_INF("Initializing OTA manager...");
    if (ota_manager_init() < 0)
    {
        LOG_ERR("❌ OTA manager init failed");
    } else {
        LOG_INF("✅ OTA manager initialized");
    }
#endif

    /* App manager (optional) - includes runtime initialization */
#ifdef CONFIG_AKIRA_APP_MANAGER
    LOG_INF("Initializing app manager...");
    if (app_manager_init() < 0)
    {
        LOG_WRN("⚠️ App manager failed");
    } else {
        LOG_INF("✅ App manager initialized");
    }
#endif

    /* Shell (optional) */
#ifdef CONFIG_AKIRA_SHELL
    LOG_INF("Initializing shell...");
    if (akira_shell_init() < 0)
    {
        LOG_WRN("⚠️ Shell init failed");
    } else {
        LOG_INF("✅ Shell initialized");
    }
#endif

    /* Web server (optional) */
#ifdef CONFIG_AKIRA_HTTP_SERVER
    LOG_INF("Starting web server...");
    if (web_server_start(NULL) < 0)
    {
        LOG_WRN("⚠️ Web server init failed");
    } else {
        LOG_INF("✅ Web server started");
    }
#endif

    LOG_INF("╔════════════════════════════════════════╗");
    LOG_INF("║     ✅ AkiraOS is ready!              ║");
    LOG_INF("╚════════════════════════════════════════╝\n");

    /* Main loop - just sleep */
    while (1)
    {
        k_sleep(K_SECONDS(10));
    }

    return 0;
}
