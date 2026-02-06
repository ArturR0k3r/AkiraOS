/**
 * @file main.c
 * @brief AkiraOS Main Entry Point
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/platform_hal.h>
#include <runtime/akira_runtime.h>
#include <runtime/app_loader/app_loader.h>
#ifdef CONFIG_FILE_SYSTEM
#include <storage/fs_manager.h>
#endif
#ifdef CONFIG_BT
#include <connectivity/bluetooth/bt_manager.h>
#endif
#include <connectivity/hid/hid_manager.h>
#ifdef CONFIG_AKIRA_BT_HID
#include <bt_hid.h>
#endif
#ifdef CONFIG_AKIRA_APP_MANAGER
#include <runtime/app_manager/app_manager.h>
#endif
#ifdef CONFIG_AKIRA_HTTP_SERVER
#include "ota/web_server.h"
#include "ota/ota_manager.h"
#endif
#ifdef CONFIG_AKIRA_SETTINGS
#include "settings/settings.h"
#endif
#ifdef CONFIG_AKIRA_USB
#include <connectivity/usb/usb_manager.h>
#endif
#ifdef CONFIG_AKIRA_USB_HID
#include <connectivity/usb/usb_hid.h>
#endif


LOG_MODULE_REGISTER(akira_main, CONFIG_AKIRA_LOG_LEVEL);

int main(void)
{
    LOG_INF("AkiraOS booting (Minimalist v1.4.x)");

    /* Initialize hardware HAL */
    if (akira_hal_init() < 0) {
        LOG_ERR("HAL init failed");
        return -ENODEV;
    }

#ifdef CONFIG_BT
    /* Initialize Bluetooth manager */
    if (bt_manager_init(NULL) < 0) {
        LOG_WRN("Bluetooth init failed - continuing without BT support");
    }
    else {
        LOG_INF("Bluetooth initialized");
    }
#endif

#ifdef CONFIG_AKIRA_BT_SHELL
    /* Initialize Bluetooth shell commands */
    if (bt_shell_init() < 0) {
        LOG_WRN("Bluetooth shell init failed");
    }
    else {
        LOG_INF("Bluetooth shell commands initialized");
    }
#endif

#ifdef CONFIG_AKIRA_USB
    if(usb_manager_init() < 0){
        LOG_WRN("USB manager init failed");
    }
    else{
        LOG_INF("USB manager initialized successfully");
    }
#endif

#ifdef CONFIG_AKIRA_HID
    if (hid_manager_init(NULL) < 0) {
        LOG_WRN("HID manager init failed");
    }
    else {
        LOG_INF("HID manager initialized");
    }
#endif

#ifdef CONFIG_AKIRA_USB_HID
    if(usb_hid_transport_init()<0){
        LOG_WRN("USB HID transport init failed");
    }
    else{
        LOG_INF("USB HID transport initialized successfully");
    }
#endif  

#ifdef CONFIG_AKIRA_BT_HID
    /* Initialize HID manager */

    if(bt_hid_init() < 0){
        LOG_WRN("Failed to init BT HID");
    }
    else{
        LOG_INF("BT HID initialized succesfully!");
    }
#endif

#ifdef CONFIG_FILE_SYSTEM
    /* Ensure filesystem is initialized for storage/backing */
    if (fs_manager_init() < 0) {
        LOG_WRN("Filesystem init failed - continuing without persistent storage");
    }
#endif


#ifdef CONFIG_AKIRA_HTTP_SERVER
    /* Initialize OTA manager before starting web server */
    if (ota_manager_init() < 0) {
        LOG_ERR("OTA manager init failed");
    }
    else {
        LOG_INF("OTA manager initialized");
    }

    if(web_server_start(NULL) < 0){
        LOG_WRN("Failed to start webserver thread!");
    }
    else{
        LOG_INF("Web server thread running!");
    }
#endif

#ifdef CONFIG_AKIRA_SETTINGS
    /* Initialize settings subsystem */
    if (akira_settings_init() < 0) {
        LOG_WRN("Settings init failed - continuing without settings support");
    }
    else{
        LOG_INF("Settings subsystem initialized");
    }
    
#endif

    /* Initialize runtime */
    if (akira_runtime_init() < 0) {
        LOG_ERR("Runtime init failed");
        return -EIO;
    }

#ifdef CONFIG_AKIRA_APP_MANAGER
    app_manager_init();
#endif

#ifdef CONFIG_AKIRA_SELFTEST
    /* Self-test (native_sim): install a dummy WASM and optional manifest */
    static const uint8_t dummy_wasm[] = {0x00, 'a', 's', 'm', 0x01, 0x00, 0x00, 0x00};
    const char *manifest = "{\"capabilities\":[\"display.write\",\"input.read\"]}";
    int sid = app_loader_install_with_manifest("selftest", dummy_wasm, sizeof(dummy_wasm), manifest, strlen(manifest));
    if (sid >= 0) {
        LOG_INF("Selftest installed as slot %d", sid);
        if (akira_runtime_start(sid) == 0) {
            LOG_INF("Selftest started (slot %d)", sid);
        } else {
            LOG_WRN("Selftest start failed (slot %d)", sid);
        }
    } else {
        LOG_WRN("Selftest install failed: %d", sid);
    }
#endif

    LOG_INF("AkiraOS init complete");

    /* Idle loop */
    while (1) {
        k_sleep(K_SECONDS(10));
    }

    return 0;
}
