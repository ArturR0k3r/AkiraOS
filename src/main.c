#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/drivers/disk.h>
#include <ff.h>
#include "drivers/display_ili9341.h"
#include "drivers/platform_hal.h"
#include "settings/settings.h"
#include "OTA/ota_manager.h"
#include "shell/akira_shell.h"
#include "OTA/web_server.h"
#include "akira/akira.h"

LOG_MODULE_REGISTER(akira_main, AKIRA_LOG_LEVEL);

static bool wifi_connected = false;
static struct net_mgmt_event_callback wifi_cb;
static FATFS fatfs;
static struct fs_mount_t fs = {
    .type = FS_FATFS,
    .fs_data = &fatfs,
    .mnt_point = "/SD:",
};

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event, struct net_if *iface);
static int initialize_wifi(void);

static void get_ip_work_handler(struct k_work *work)
{
    if (!wifi_connected)
    {
        return;
    }

    struct net_if *iface = net_if_get_default();
    if (!iface)
    {
        LOG_ERR("No default network interface");
        return;
    }

    char addr_str[NET_IPV4_ADDR_LEN];

    /* Get preferred IPv4 global address (assigned via DHCP or static) */
    struct in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (addr)
    {
        net_addr_ntop(AF_INET, addr, addr_str, sizeof(addr_str));
        LOG_INF("IP Address: %s", addr_str);
        web_server_notify_network_status(true, addr_str);
        return;
    }

    LOG_WRN("No valid IP address found");
}

static K_WORK_DELAYABLE_DEFINE(ip_work, get_ip_work_handler);

static int get_system_info_callback(char *buffer, size_t buffer_size)
{
    struct system_stats stats;
    if (shell_get_system_stats(&stats) != 0)
    {
        return -1;
    }

    return snprintf(buffer, buffer_size,
                    "{"
                    "\"uptime\":\"%llu ms\","
                    "\"memory\":\"%zu/%zu KB\","
                    "\"wifi\":\"%s\","
                    "\"cpu\":\"%u%%\","
                    "\"temp\":\"%d°C\","
                    "\"threads\":\"%u\""
                    "}",
                    stats.uptime_ms,
                    stats.heap_used / 1024,
                    (stats.heap_used + stats.heap_free) / 1024,
                    stats.wifi_connected ? "Connected" : "Disconnected",
                    stats.cpu_usage_percent,
                    stats.temperature_celsius,
                    stats.thread_count);
}

static int get_button_state_callback(char *buffer, size_t buffer_size)
{
    uint32_t button_state = shell_read_buttons();

    return snprintf(buffer, buffer_size,
                    "{"
                    "\"power\":%s,"
                    "\"settings\":%s,"
                    "\"up\":%s,"
                    "\"down\":%s,"
                    "\"left\":%s,"
                    "\"right\":%s,"
                    "\"a\":%s,"
                    "\"b\":%s,"
                    "\"x\":%s,"
                    "\"y\":%s"
                    "}",
                    (button_state & BTN_ONOFF) ? "true" : "false",
                    (button_state & BTN_SETTINGS) ? "true" : "false",
                    (button_state & BTN_UP) ? "true" : "false",
                    (button_state & BTN_DOWN) ? "true" : "false",
                    (button_state & BTN_LEFT) ? "true" : "false",
                    (button_state & BTN_RIGHT) ? "true" : "false",
                    (button_state & BTN_A) ? "true" : "false",
                    (button_state & BTN_B) ? "true" : "false",
                    (button_state & BTN_X) ? "true" : "false",
                    (button_state & BTN_Y) ? "true" : "false");
}

static int initialize_sd_card(void)
{
    int ret;
    static const char *disk_pdrv = "SD";

    LOG_INF("Initializing SD card...");

    ret = disk_access_init(disk_pdrv);
    if (ret != 0)
    {
        LOG_ERR("SD card initialization failed: %d", ret);
        return ret;
    }

    ret = fs_mount(&fs);
    if (ret != 0)
    {
        LOG_ERR("SD card mount failed: %d", ret);
        return ret;
    }

    LOG_INF("✅ SD card mounted successfully at %s", fs.mnt_point);
    return 0;
}

static int get_settings_info_callback(char *buffer, size_t buffer_size)
{
    return user_settings_to_json(buffer, buffer_size);
}

static int execute_shell_command_callback(const char *command, char *response, size_t response_size)
{
    if (strcmp(command, "help") == 0)
    {
        return snprintf(response, response_size,
                        "Available commands:\n"
                        "  help - Show this help\n"
                        "  sys info - System information\n"
                        "  wifi status - WiFi connection status\n"
                        "  web status - Web server status\n"
                        "  ota status - OTA update status\n"
                        "  settings show - Show current settings\n"
                        "  game buttons - Read button states\n"
                        "  debug threads - Thread information\n"
                        "  reboot - Reboot device");
    }
    else if (strcmp(command, "sys info") == 0 || strcmp(command, "sysinfo") == 0)
    {
        struct system_stats stats;
        if (shell_get_system_stats(&stats) == 0)
        {
            return snprintf(response, response_size,
                            "Uptime: %llu ms\n"
                            "Memory: %zu/%zu KB used\n"
                            "WiFi: %s\n"
                            "IP: %s\n"
                            "CPU: %u%%",
                            stats.uptime_ms,
                            stats.heap_used / 1024,
                            (stats.heap_used + stats.heap_free) / 1024,
                            stats.wifi_connected ? "Connected" : "Disconnected",
                            stats.wifi_connected ? "192.168.100.248" : "N/A",
                            stats.cpu_usage_percent);
        }
        return snprintf(response, response_size, "Failed to get system stats");
    }
    else if (strcmp(command, "wifi status") == 0 || strcmp(command, "wifi") == 0)
    {
        struct system_stats stats;
        shell_get_system_stats(&stats);
        const struct user_settings *settings = user_settings_get();
        return snprintf(response, response_size,
                        "WiFi Status:\n"
                        "  State: %s\n"
                        "  SSID: %s\n"
                        "  IP: %s\n"
                        "  Enabled: %s",
                        stats.wifi_connected ? "Connected" : "Disconnected",
                        settings->wifi_ssid,
                        stats.wifi_connected ? "192.168.100.248" : "N/A",
                        settings->wifi_enabled ? "Yes" : "No");
    }
    else if (strcmp(command, "web status") == 0 || strcmp(command, "web") == 0)
    {
        struct web_server_stats ws_stats;
        web_server_get_stats(&ws_stats);
        return snprintf(response, response_size,
                        "Web Server Status:\n"
                        "  State: %s\n"
                        "  URL: http://192.168.100.248:8080/\n"
                        "  Requests: %u\n"
                        "  Connections: %u",
                        ws_stats.state == WEB_SERVER_RUNNING ? "Running" : "Stopped",
                        ws_stats.requests_handled,
                        ws_stats.active_connections);
    }
    else if (strcmp(command, "game buttons") == 0 || strcmp(command, "buttons") == 0)
    {
        uint32_t buttons = shell_read_buttons();
        return snprintf(response, response_size,
                        "Button states: 0x%08x\n"
                        "Power: %s, Settings: %s\n"
                        "D-Pad: U=%s D=%s L=%s R=%s\n"
                        "Actions: A=%s B=%s X=%s Y=%s",
                        buttons,
                        (buttons & BTN_ONOFF) ? "ON" : "off",
                        (buttons & BTN_SETTINGS) ? "ON" : "off",
                        (buttons & BTN_UP) ? "ON" : "off",
                        (buttons & BTN_DOWN) ? "ON" : "off",
                        (buttons & BTN_LEFT) ? "ON" : "off",
                        (buttons & BTN_RIGHT) ? "ON" : "off",
                        (buttons & BTN_A) ? "ON" : "off",
                        (buttons & BTN_B) ? "ON" : "off",
                        (buttons & BTN_X) ? "ON" : "off",
                        (buttons & BTN_Y) ? "ON" : "off");
    }
    else if (strcmp(command, "settings show") == 0 || strcmp(command, "settings") == 0)
    {
        const struct user_settings *settings = user_settings_get();
        return snprintf(response, response_size,
                        "Settings:\n"
                        "  Device ID: %s\n"
                        "  WiFi SSID: %s\n"
                        "  WiFi Enabled: %s\n"
                        "  WiFi Password: %s",
                        settings->device_id,
                        settings->wifi_ssid,
                        settings->wifi_enabled ? "Yes" : "No",
                        strlen(settings->wifi_passcode) > 0 ? "***" : "(not set)");
    }
    else if (strcmp(command, "ota status") == 0 || strcmp(command, "ota") == 0)
    {
        const struct ota_progress *progress = ota_get_progress();
        return snprintf(response, response_size,
                        "OTA Status:\n"
                        "  State: %s\n"
                        "  Progress: %d%%\n"
                        "  Written: %zu / %zu bytes\n"
                        "  Message: %s",
                        ota_state_to_string(progress->state),
                        progress->percentage,
                        progress->bytes_written,
                        progress->total_size,
                        progress->status_message);
    }
    else if (strcmp(command, "debug threads") == 0 || strcmp(command, "threads") == 0)
    {
        return snprintf(response, response_size,
                        "Active Threads:\n"
                        "  main (prio 0)\n"
                        "  web_server (prio 7)\n"
                        "  ota_manager (prio 6)\n"
                        "  settings (prio 7)\n"
                        "  logging (prio 10)\n"
                        "  idle (prio 15)");
    }
    else if (strcmp(command, "reboot") == 0)
    {
        snprintf(response, response_size, "Rebooting in 2 seconds...");
        k_msleep(100); /* Let response be sent */
        sys_reboot(SYS_REBOOT_COLD);
        return 0;
    }
    else if (strcmp(command, "version") == 0)
    {
        return snprintf(response, response_size,
                        "AkiraOS v1.2.0-OTA\n"
                        "Build: %s %s\n"
                        "Board: ESP32-S3 DevKitM",
                        __DATE__, __TIME__);
    }
    else if (command[0] == '\0')
    {
        return 0; /* Empty command */
    }

    return snprintf(response, response_size,
                    "Unknown command: '%s'\nType 'help' for available commands",
                    command);
}

/* WiFi event handling */
static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event,
                               struct net_if *netif)
{
    switch (mgmt_event)
    {
    case NET_EVENT_WIFI_CONNECT_RESULT:
    {
        const struct wifi_status *status = (const struct wifi_status *)cb->info;
        if (status->status == 0)
        {
            LOG_INF("WiFi connected successfully");
            wifi_connected = true;
            // Get IP address when connected
            k_work_schedule(&ip_work, K_SECONDS(2));
        }
        else
        {
            LOG_ERR("WiFi connection failed: %d", status->status);
            wifi_connected = false;
        }
        break;
    }

    case NET_EVENT_WIFI_DISCONNECT_RESULT:
        LOG_INF("WiFi disconnected");
        wifi_connected = false;
        // Notify web server of disconnection
        web_server_notify_network_status(false, NULL);
        break;

    case NET_EVENT_IPV4_ADDR_ADD:
        LOG_INF("IPv4 address assigned");
        if (wifi_connected)
        {
            struct in_addr *addr;
            char addr_str[NET_IPV4_ADDR_LEN];

            addr = net_if_ipv4_get_global_addr(netif, NET_ADDR_PREFERRED);
            if (addr)
            {
                net_addr_ntop(AF_INET, addr, addr_str, sizeof(addr_str));
                LOG_INF("Got IP address: %s", addr_str);
                web_server_notify_network_status(true, addr_str);
            }
            else
            {
                LOG_WRN("No preferred IPv4 address found");
            }
        }
        break;

    case NET_EVENT_IPV4_ADDR_DEL:
        LOG_INF("IPv4 address removed");
        web_server_notify_network_status(false, NULL);
        break;
    }
}

static int initialize_wifi(void)
{
    /* Check if WiFi hardware is available */
    if (!akira_has_wifi())
    {
        LOG_INF("WiFi not available on this platform - skipping");
        return 0;
    }

    struct net_if *iface = net_if_get_default();
    if (!iface)
    {
        LOG_ERR("No default network interface found");
        return -ENODEV;
    }

    net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
                                 NET_EVENT_WIFI_CONNECT_RESULT |
                                     NET_EVENT_WIFI_DISCONNECT_RESULT |
                                     NET_EVENT_IPV4_ADDR_ADD |
                                     NET_EVENT_IPV4_ADDR_DEL);

    net_mgmt_add_event_callback(&wifi_cb);

    const struct user_settings *settings = user_settings_get();

    if (!settings->wifi_enabled)
    {
        LOG_INF("WiFi disabled in settings");
        return 0;
    }

    if (strlen(settings->wifi_ssid) == 0)
    {
        LOG_WRN("No WiFi SSID configured - use 'settings set_wifi <ssid> <password>'");
        return -EINVAL;
    }

    struct wifi_connect_req_params wifi_params = {
        .ssid = (uint8_t *)settings->wifi_ssid,
        .ssid_length = strlen(settings->wifi_ssid),
        .psk = (uint8_t *)settings->wifi_passcode,
        .psk_length = strlen(settings->wifi_passcode),
        .channel = WIFI_CHANNEL_ANY,
        .security = strlen(settings->wifi_passcode) > 0 ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
        .mfp = WIFI_MFP_OPTIONAL,
    };

    LOG_INF("Connecting to WiFi: %s", settings->wifi_ssid);

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &wifi_params, sizeof(wifi_params));
    if (ret)
    {
        LOG_ERR("WiFi connection request failed: %d", ret);
        return ret;
    }

    return 0;
}

static void on_settings_changed(const char *key, const void *value, void *user_data)
{
    LOG_INF("Setting changed: %s", key);

    if (strcmp(key, WIFI_SSID_KEY) == 0 || strcmp(key, WIFI_PASSCODE_KEY) == 0)
    {
        LOG_INF("WiFi credentials updated - reconnecting...");
        if (user_settings_get()->wifi_enabled)
        {
            initialize_wifi();
        }
    }
    else if (strcmp(key, WIFI_ENABLED_KEY) == 0)
    {
        bool enabled = *(bool *)value;
        LOG_INF("WiFi %s via settings", enabled ? "enabled" : "disabled");

        if (enabled && !wifi_connected)
        {
            initialize_wifi();
        }
    }
}

static void on_ota_progress(const struct ota_progress *progress, void *user_data)
{
    static uint8_t last_percentage = 255;
    char log_msg[128];

    if (progress->state == OTA_STATE_IN_PROGRESS && last_percentage == 255)
    {
        LOG_INF("OTA update started...");
        web_server_add_log("<inf> ota: Update started, receiving firmware...");
    }

    if (progress->percentage != last_percentage)
    {
        LOG_INF("OTA: %s (%d%%)", progress->status_message, progress->percentage);

        /* Log every 10% to web terminal */
        if (progress->percentage % 10 == 0 || progress->percentage > 95)
        {
            snprintf(log_msg, sizeof(log_msg), "<inf> ota: Progress %d%% (%zu/%zu bytes)",
                     progress->percentage, progress->bytes_written, progress->total_size);
            web_server_add_log(log_msg);
        }
        last_percentage = progress->percentage;
    }

    if (progress->state == OTA_STATE_ERROR)
    {
        LOG_ERR("OTA Error: %s", progress->status_message);
        snprintf(log_msg, sizeof(log_msg), "<err> ota: %s", progress->status_message);
        web_server_add_log(log_msg);
    }
    else if (progress->state == OTA_STATE_COMPLETE)
    {
        LOG_INF("✅ OTA Complete - reboot to apply new firmware");
        web_server_add_log("<inf> ota: Update complete!");
        web_server_add_log("<inf> ota: Rebooting to apply new firmware...");
    }
}

int main(void)
{
    printk("=== AkiraOS main() started ===\n");
    int ret;

    /* Initialize Akira HAL */
    ret = akira_hal_init();
    if (ret < 0)
    {
        LOG_ERR("Akira HAL initialization failed: %d", ret);
        return ret;
    }

    LOG_INF("Platform: %s", akira_get_platform_name());
    LOG_INF("Display: %s", akira_has_display() ? "Available" : "Not Available");
    LOG_INF("WiFi: %s", akira_has_wifi() ? "Available" : "Not Available");
    LOG_INF("SPI: %s", akira_has_spi() ? "Available" : "Not Available");

    const struct device *gpio_dev = NULL;
    const struct device *spi_dev = NULL;
    struct spi_config spi_cfg = {0};

    /* Initialize hardware if available */
    if (akira_has_display())
    {
        gpio_dev = akira_get_gpio_device("gpio0");
        spi_dev = akira_get_spi_device("spi2");

        if (!gpio_dev)
        {
            LOG_ERR("GPIO device not available");
        }
        if (!spi_dev)
        {
            LOG_ERR("SPI device not available");
        }

        if (gpio_dev && spi_dev)
        {
            ret = akira_gpio_pin_configure(gpio_dev, ILI9341_CS_PIN, GPIO_OUTPUT_ACTIVE);
            if (ret < 0)
            {
                LOG_ERR("Failed to configure CS pin: %d", ret);
            }

            ret = akira_gpio_pin_configure(gpio_dev, ILI9341_DC_PIN, GPIO_OUTPUT_ACTIVE);
            if (ret < 0)
            {
                LOG_ERR("Failed to configure DC pin: %d", ret);
            }

            ret = akira_gpio_pin_configure(gpio_dev, ILI9341_RESET_PIN, GPIO_OUTPUT_ACTIVE);
            if (ret < 0)
            {
                LOG_ERR("Failed to configure RESET pin: %d", ret);
            }

            ret = akira_gpio_pin_configure(gpio_dev, ILI9341_BL_PIN, GPIO_OUTPUT_ACTIVE);
            if (ret < 0)
            {
                LOG_ERR("Failed to configure backlight pin: %d", ret);
            }

            akira_gpio_pin_set(gpio_dev, ILI9341_CS_PIN, 1);
            akira_gpio_pin_set(gpio_dev, ILI9341_DC_PIN, 0);
            akira_gpio_pin_set(gpio_dev, ILI9341_BL_PIN, 1);

            printk("Performing hardware reset...\n");
            akira_gpio_pin_set(gpio_dev, ILI9341_RESET_PIN, 1);
            k_msleep(10);
            akira_gpio_pin_set(gpio_dev, ILI9341_RESET_PIN, 0);
            k_msleep(10);
            akira_gpio_pin_set(gpio_dev, ILI9341_RESET_PIN, 1);
            k_msleep(120);

            spi_cfg.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER;
            spi_cfg.frequency = 10000000;
            spi_cfg.slave = 0;

            printk("spi_cfg: freq=%u, op=0x%08x, slave=%d\n",
                   spi_cfg.frequency, spi_cfg.operation, spi_cfg.slave);

            akira_gpio_pin_set(gpio_dev, ILI9341_CS_PIN, 0);
            akira_gpio_pin_set(gpio_dev, ILI9341_DC_PIN, 0);
            k_usleep(1);

            uint8_t reset_cmd = 0x01;
            struct spi_buf tx_buf = {.buf = &reset_cmd, .len = 1};
            struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};

            ret = akira_spi_write(spi_dev, &spi_cfg, &tx_bufs);

            k_usleep(1);
            akira_gpio_pin_set(gpio_dev, ILI9341_CS_PIN, 1);

            if (ret < 0)
            {
                LOG_ERR("SPI write failed: %d", ret);
            }

            k_msleep(150);

            ret = ili9341_init(spi_dev, gpio_dev, &spi_cfg);
            if (ret < 0)
            {
                LOG_ERR("Display initialization failed: %d", ret);
            }
            else
            {
                LOG_INF("✅ ILI9341 display initialized");
                LOG_INF("=== AkiraOS v1.2.0 Test ===");
                ili9341_draw_text(10, 30, "=== AkiraOS v1.2.0 ===", BLACK_COLOR, FONT_7X10);
                LOG_INF("Hardware platform: %s", akira_get_platform_name());
                char platform_text[64];
                snprintf(platform_text, sizeof(platform_text), "Platform: %s", akira_get_platform_name());
                ili9341_draw_text(10, 50, platform_text, BLACK_COLOR, FONT_7X10);
                LOG_INF("Features: OTA Updates, Web Interface, Gaming Controls");
                ili9341_draw_text(10, 70, "Features: OTA Updates, Web Interface", BLACK_COLOR, FONT_7X10);
            }
        }
    }
    else
    {
        LOG_INF("Display hardware not available");
    }

    LOG_INF("Build: %s %s", __DATE__, __TIME__);

    // Initialize SD card
    ret = initialize_sd_card();
    if (ret)
    {
        LOG_WRN("SD card initialization failed: %d - continuing without SD card", ret);
    }
    else
    {
        LOG_INF("✅ SD card initialized");
    }

    // Initialize settings
    ret = user_settings_init();
    if (ret)
    {
        LOG_ERR("Settings initialization failed: %d", ret);
    }
    LOG_INF("✅ Settings module initialized");

    // Register settings change callback
    user_settings_register_callback(on_settings_changed, NULL);

    // Initialize OTA manager
    ret = ota_manager_init();
    if (ret)
    {
        LOG_ERR("OTA manager initialization failed: %d", ret);
    }
    LOG_INF("✅ OTA manager initialized");

    // Register OTA progress callback
    ota_register_progress_callback(on_ota_progress, NULL);

    // Initialize Akira shell
    ret = akira_shell_init();
    if (ret)
    {
        LOG_ERR("Akira shell initialization failed: %d", ret);
    }
    LOG_INF("✅ Akira shell initialized");

    // Prepare web server callbacks
    struct web_server_callbacks web_callbacks = {
        .get_system_info = get_system_info_callback,
        .get_button_state = get_button_state_callback,
        .get_settings_info = get_settings_info_callback,
        .execute_shell_command = execute_shell_command_callback};

    // Start web server
    ret = web_server_start(&web_callbacks);
    if (ret)
    {
        LOG_ERR("Web server initialization failed: %d", ret);
    }
    LOG_INF("✅ Web server initialized and started");

    // Initialize WiFi
    ret = initialize_wifi();
    if (ret)
    {
        LOG_WRN("WiFi initialization failed: %d - continuing without WiFi", ret);
        LOG_INF("💡 Configure WiFi: settings set_wifi <ssid> <password>");
    }
    else
    {
        LOG_INF("✅ WiFi initialization started");
    }

    /* Add startup logs to web terminal */
    web_server_add_log("<inf> AkiraOS v1.2.0 started");
    web_server_add_log("<inf> Build: " __DATE__ " " __TIME__);
    web_server_add_log("<inf> Type 'help' for available commands");

    uint32_t loop_count = 0;
    while (1)
    {
        loop_count++;

        /* Periodic status log every 60 seconds */
        if (loop_count % 6 == 0)
        {
            char status_msg[128];
            uint64_t uptime = k_uptime_get() / 1000;
            snprintf(status_msg, sizeof(status_msg),
                     "<inf> Heartbeat: uptime=%llus, loops=%u",
                     uptime, loop_count);
            web_server_add_log(status_msg);
        }

        LOG_INF("... AkiraOS main loop running ...");
        k_sleep(K_SECONDS(10));
    }

    return 0;
}
