/**
 * @file main.c
 * @brief AkiraOS Main Entry Point
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/platform_hal.h>
#include <api/akira_display_api.h>
#include <runtime/akira_runtime.h>
#include <runtime/app_loader/app_loader.h>
#include <runtime/security/sandbox.h>
#include <runtime/runtime_cache.h>
#ifdef CONFIG_FILE_SYSTEM
#include <storage/fs_manager.h>
#endif
#ifdef CONFIG_BT
#include <connectivity/bluetooth/bt_manager.h>
#if defined(CONFIG_AKIRA_BT_COMPANION)
#include <connectivity/bluetooth/companion_service.h>
#endif
#endif
#include <connectivity/hid/hid_manager.h>
#ifdef CONFIG_AKIRA_BT_HID
#include <bt_hid.h>
#endif
#ifdef CONFIG_AKIRA_APP_MANAGER
#include <runtime/app_manager/app_manager.h>
#endif
#ifdef CONFIG_AKIRA_SD_CARD
#include <connectivity/storage/sd_manager.h>
#include <connectivity/storage/usb_storage.h>
#endif
#ifdef CONFIG_AKIRA_INPUT_API
#include <api/akira_input_api.h>
#endif
#ifdef CONFIG_AKIRA_HTTP_SERVER
#include "http_server.h"
#include "http_routes.h"
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
#ifdef CONFIG_AKIRA_HID_APP_HANDLER
#include <connectivity/hid/hid_app_handler.h>
#endif
#ifdef CONFIG_AKIRA_MODULE_RF
#include <api/akira_rf_api.h>
#endif

LOG_MODULE_REGISTER(akira_main, CONFIG_AKIRA_LOG_LEVEL);

int main(void)
{
    LOG_INF("=====================================================)");
    LOG_INF("AkiraOS booting (v1.5.8)");
    LOG_INF("Platform: %s", akira_get_platform_name());
    LOG_INF("Build: %s %s", __DATE__, __TIME__);
    LOG_INF("=====================================================)");
    
    /* Initialize hardware HAL */
    if (akira_hal_init() < 0)
    {
        LOG_ERR("HAL init failed");
        return -ENODEV;
    }

    /* Display test - runs AFTER HAL initialization */
#ifdef CONFIG_DISPLAY
    akira_display_clear(0x0021);
    /* Display boot info on screen */
    char buf[64];
    int y_pos = 20;
    const int line_height = 12;
    const uint16_t text_color = 0xFFFF; // White

    akira_display_text(5, y_pos, "====================================", text_color);
    y_pos += line_height;

    akira_display_text(5, y_pos, "AkiraOS booting", text_color);
    y_pos += line_height;

    akira_display_text(5, y_pos, "Minimalist v" CONFIG_AKIRA_OS_VERSION, text_color);
    y_pos += line_height;

    snprintf(buf, sizeof(buf), "Platform: %s", akira_get_platform_name());
    akira_display_text(5, y_pos, buf, text_color);
    y_pos += line_height;

    snprintf(buf, sizeof(buf), "Build: %s", __DATE__);
    akira_display_text(5, y_pos, buf, text_color);
    y_pos += line_height;

    snprintf(buf, sizeof(buf), "       %s", __TIME__);
    akira_display_text(5, y_pos, buf, text_color);
    y_pos += line_height;

    akira_display_text(5, y_pos, "====================================", text_color);

    akira_display_flush();
    // k_sleep(K_MSEC(CONFIG_AKIRA_BOOT_DELAY_MS));

#endif

    /* USB manager auto-initialized via SYS_INIT (see usb_manager.c) */

#ifdef CONFIG_AKIRA_HID
    if (hid_manager_init(NULL) < 0)
    {
        LOG_WRN("HID manager init failed");
    }
    else
    {
        LOG_INF("HID manager initialized");
    }
#endif

#ifdef CONFIG_AKIRA_USB_HID
    if (usb_hid_transport_init() < 0)
    {
        LOG_WRN("USB HID transport init failed");
    }
    else
    {
        LOG_INF("USB HID transport initialized successfully");
    }
#endif

#ifdef CONFIG_AKIRA_HID_APP_HANDLER
    if (hid_app_handler_init() < 0)
    {
        LOG_WRN("HID app handler init failed");
    }
    else
    {
        LOG_INF("HID app handler initialized (WebHID Report ID 3)");
    }
#endif

#ifdef CONFIG_AKIRA_BT_HID
    /* Initialize HID manager */

    if (bt_hid_init() < 0)
    {
        LOG_WRN("Failed to init BT HID");
    }
    else
    {
        LOG_INF("BT HID initialized succesfully!");
    }
#endif

#if defined(CONFIG_AKIRA_USB_HID) && defined(CONFIG_AKIRA_HID)
    /* Auto-enable USB HID so the device enumerates on the host at boot.
     * USB transport was registered + finalized in usb_hid_transport_init();
     * calling hid_manager_enable() here triggers usbd_enable() to start USB. */
    hid_manager_set_transport(HID_TRANSPORT_USB);
    if (hid_manager_enable() < 0)
    {
        LOG_WRN("USB HID auto-enable failed");
    }
    else
    {
        LOG_INF("USB HID enabled at boot");
    }
#endif


#ifdef CONFIG_BT
#if defined(CONFIG_AKIRA_BT_COMPANION)
    if (bt_manager_boot_mode_is_companion())
    {
        /* Companion service owns the radio in this mode. Ensure it is up (the
         * SYS_INIT hook may already have started it) and do NOT run the default
         * HID bring-up, which would re-init BT and start HID advertising. */
        if (companion_svc_is_ready() || companion_svc_init() == 0)
        {
            LOG_INF("BT boot mode: companion");
        }
        else
        {
            LOG_WRN("Companion start failed; falling back to default BT");
            bt_manager_init(NULL);
        }
    }
    else
#endif
    if(bt_manager_init(NULL) < 0)
    {
        LOG_WRN("Bluetooth manager init failed");
    }
    else
    {
        LOG_INF("Bluetooth manager initialized");
    }
#endif

    /* Filesystem auto-initialized via SYS_INIT (see fs_manager.c) */

#ifdef CONFIG_AKIRA_MODULE_RF
    LOG_INF("RF module enabled");
#ifdef CONFIG_AKIRA_CC1121
    if (akira_rf_init(AKIRA_RF_CHIP_CC1121) < 0) {
        LOG_ERR("CC1121 init failed");
    }
#endif
#ifdef CONFIG_AKIRA_LR2021
    if (akira_rf_init(AKIRA_RF_CHIP_LR2021) < 0) {
        LOG_ERR("LR2021 init failed");
    }
#endif
#endif

#if defined(CONFIG_AKIRA_HTTP_SERVER)
#if defined(CONFIG_AKIRA_OTA)
    /* Initialize OTA manager (required by /upload endpoint) */
    if (ota_manager_init() < 0)
    {
        LOG_ERR("OTA manager init failed");
    }
    else
    {
        LOG_INF("OTA manager initialized");
    }
#endif

    if (akira_http_server_init() < 0)
    {
        LOG_ERR("HTTP server init failed");
    }
    else if (akira_http_routes_init() < 0)
    {
        LOG_ERR("HTTP routes init failed");
    }
    else if (akira_http_server_start() < 0)
    {
        LOG_WRN("Failed to start HTTP server!");
    }
    else
    {
        LOG_INF("HTTP server started on port %d", HTTP_SERVER_PORT);
    }
#endif

    /* Settings auto-initialized via SYS_INIT (see settings.c) */

    /* Initialize runtime */
    if (akira_runtime_init() < 0)
    {
        LOG_ERR("Runtime init failed");
        return -EIO;
    }

    /* Log security status */
    LOG_INF("Security: sandbox=%s, signing=%s, integrity=%s, cache=%s",
#ifdef CONFIG_AKIRA_SANDBOX
            "enabled",
#else
            "disabled",
#endif
#ifdef CONFIG_AKIRA_APP_SIGNING
            "enforced",
#else
            "permissive",
#endif
#ifdef CONFIG_AKIRA_WASM_INTEGRITY_CHECK
            "enabled",
#else
            "disabled",
#endif
#ifdef CONFIG_AKIRA_MODULE_CACHE
            "enabled"
#else
            "disabled"
#endif
    );

#ifdef CONFIG_AKIRA_APP_MANAGER
    app_manager_init();
#endif

#ifdef CONFIG_AKIRA_SD_CARD
    sd_manager_init();
#endif

#ifdef CONFIG_AKIRA_APP_SOURCE_USB
    usb_storage_init();
#endif

#ifdef CONFIG_AKIRA_INPUT_API
    akira_input_init();
#endif

#ifdef CONFIG_AKIRA_BOOT_SHELL
    {
        int r = app_manager_start(CONFIG_AKIRA_BOOT_SHELL_NAME);
        if (r < 0) {
            LOG_WRN("Boot shell '%s' not found (%d) — install via OTA/UART",
                    CONFIG_AKIRA_BOOT_SHELL_NAME, r);
        } else {
            LOG_INF("Boot shell '%s' started", CONFIG_AKIRA_BOOT_SHELL_NAME);
        }
    }
#endif

#ifdef CONFIG_AKIRA_SELFTEST
    /* Self-test (native_sim): install a dummy WASM and optional manifest */
    static const uint8_t dummy_wasm[] = {0x00, 'a', 's', 'm', 0x01, 0x00, 0x00, 0x00};
    const char *manifest = "{\"capabilities\":[\"display.write\",\"input.read\"]}";
    int sid = app_loader_install_with_manifest("selftest", dummy_wasm, sizeof(dummy_wasm), manifest, strlen(manifest));
    if (sid >= 0)
    {
        LOG_INF("Selftest installed as slot %d", sid);
        if (akira_runtime_start(sid) == 0)
        {
            LOG_INF("Selftest started (slot %d)", sid);
        }
        else
        {
            LOG_WRN("Selftest start failed (slot %d)", sid);
        }
    }
    else
    {
        LOG_WRN("Selftest install failed: %d", sid);
    }
#endif

    LOG_INF("AkiraOS init complete");
    k_sleep(K_MSEC(1000));  // Brief pause before entering main loop

#if defined(CONFIG_DISPLAY) && !defined(CONFIG_AKIRA_OS_SHELL)
    /* Idle loop — main() owns the display and animates a status screen. */
    while (1) {
        extern akira_managed_app_t g_apps[AKIRA_MAX_WASM_INSTANCES];
        static uint32_t frame = 0;
        static bool idle_screen_shown = false;
        char buf[64];
        const uint16_t text_color = 0xFFFF; // White
        const uint16_t bg_color = 0x0000;   // Black

        /* Check if any app is running */
        bool app_running = false;
        for (int i = 0; i < AKIRA_MAX_WASM_INSTANCES; i++)
        {
            if (g_apps[i].used && g_apps[i].status == AKIRA_APP_STATUS_RUNNING)
            {
                app_running = true;
                break;
            }
        }

#if defined(CONFIG_AKIRA_BOOT_SHELL) && defined(CONFIG_AKIRA_BOOT_SHELL_RESTART)
        {
            /* Keep the debounce timer fresh while any app is running so the
             * 1.5 s window only starts counting AFTER the last app stops.
             * Without this, s_last_shell_ms stays at 0 and the restart fires
             * immediately after app_switch() exits the shell — racing with the
             * deferred app_switch work and stealing the only WASM slot. */
            static int64_t s_last_shell_ms;
            int64_t now_ms = k_uptime_get();
            if (app_running) {
                s_last_shell_ms = now_ms;  /* reset: app still alive */
            } else if (now_ms - s_last_shell_ms >= 1500) {
                s_last_shell_ms = now_ms;
                LOG_INF("No app running — restarting '%s'", CONFIG_AKIRA_BOOT_SHELL_NAME);
                int sr = app_manager_start(CONFIG_AKIRA_BOOT_SHELL_NAME);
                if (sr < 0) {
                    LOG_WRN("Shell restart failed (%d)", sr);
                } else {
                    k_sleep(K_MSEC(200));
                    app_running = true;
                }
            }
        }
#endif /* CONFIG_AKIRA_BOOT_SHELL && CONFIG_AKIRA_BOOT_SHELL_RESTART */

        if (!app_running) {
            /* No app running - show system info animation */
            idle_screen_shown = true;
            akira_display_clear(bg_color);

            akira_display_text(5, 10, "=== AkiraOS System ===", text_color);

            snprintf(buf, sizeof(buf), "Platform: %s", akira_get_platform_name());
            akira_display_text(5, 25, buf, text_color);

            snprintf(buf, sizeof(buf), "Uptime: %llu s", k_uptime_get() / 1000);
            akira_display_text(5, 40, buf, text_color);

            const char *spinner[] = {"◐", "◓", "◑", "◒"};
            const char *status[] = {"●", "○"};

            snprintf(buf, sizeof(buf), "%s Ready %s", spinner[frame % 4], status[(frame / 2) % 2]);
            akira_display_text(5, 55, buf, text_color);

            snprintf(buf, sizeof(buf), "[");
            for (int i = 0; i < 16; i++)
            {
                buf[i + 1] = (i < (frame % 16)) ? '=' : '-';
            }
            buf[17] = ']';
            buf[18] = '\0';
            akira_display_text(5, 70, buf, text_color);

            frame++;
            akira_display_flush();
        }
        else if (idle_screen_shown)
        {
            /* App just started - clear the idle screen once */
            akira_display_clear(bg_color);
            akira_display_flush();
            idle_screen_shown = false;
            frame = 0;
        }

        k_sleep(K_MSEC(100));
        // may be add show to display all installed apps and add posibility to run them from there? or just show some system info and status?
        // and if display available show some nice animation or something?
    }
#else
    /* Nothing for main() to do — the OS shell thread (or a headless build) owns
     * the device from here.  Block forever rather than spinning at 10 Hz: a
     * timed k_sleep() wakes the CPU and reprograms the tick 10x/s for the life
     * of the device, which is pure battery drain.  K_FOREVER keeps main's stack
     * reserved (other code holds pointers into its frame) while never running. */
    k_sleep(K_FOREVER);
#endif

    return 0;
}
