#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
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
#include "drivers/akira_hal.h"
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
                        "• sys info - System information\n"
                        "• game buttons - Read button states\n"
                        "• settings show - Show current settings\n"
                        "• ota status - OTA status\n"
                        "• debug threads - Thread information");
    }
    else if (strcmp(command, "sys info") == 0)
    {
        struct system_stats stats;
        if (shell_get_system_stats(&stats) == 0)
        {
            return snprintf(response, response_size,
                            "Uptime: %llu ms\n"
                            "Memory: %zu/%zu KB\n"
                            "WiFi: %s\n"
                            "CPU: %u%%\n"
                            "Temperature: %d°C",
                            stats.uptime_ms,
                            stats.heap_used / 1024,
                            (stats.heap_used + stats.heap_free) / 1024,
                            stats.wifi_connected ? "Connected" : "Disconnected",
                            stats.cpu_usage_percent,
                            stats.temperature_celsius);
        }
    }
    else if (strcmp(command, "game buttons") == 0)
    {
        uint32_t buttons = shell_read_buttons();
        return snprintf(response, response_size,
                        "Button states:\n"
                        "Power: %s, Settings: %s\n"
                        "D-Pad: U=%s D=%s L=%s R=%s\n"
                        "Actions: A=%s B=%s X=%s Y=%s",
                        (buttons & BTN_ONOFF) ? "PRESSED" : "Released",
                        (buttons & BTN_SETTINGS) ? "PRESSED" : "Released",
                        (buttons & BTN_UP) ? "PRESSED" : "Released",
                        (buttons & BTN_DOWN) ? "PRESSED" : "Released",
                        (buttons & BTN_LEFT) ? "PRESSED" : "Released",
                        (buttons & BTN_RIGHT) ? "PRESSED" : "Released",
                        (buttons & BTN_A) ? "PRESSED" : "Released",
                        (buttons & BTN_B) ? "PRESSED" : "Released",
                        (buttons & BTN_X) ? "PRESSED" : "Released",
                        (buttons & BTN_Y) ? "PRESSED" : "Released");
    }
    else if (strcmp(command, "settings show") == 0)
    {
        const struct user_settings *settings = user_settings_get();
        return snprintf(response, response_size,
                        "Device ID: %s\n"
                        "WiFi SSID: %s\n"
                        "WiFi Enabled: %s\n"
                        "WiFi Password: %s",
                        settings->device_id,
                        settings->wifi_ssid,
                        settings->wifi_enabled ? "Yes" : "No",
                        strlen(settings->wifi_passcode) > 0 ? "***SET***" : "***NOT SET***");
    }
    else if (strcmp(command, "ota status") == 0)
    {
        const struct ota_progress *progress = ota_get_progress();

        return snprintf(response, response_size,
                        "OTA State: %s\n"
                        "Progress: %d%% (%zu/%zu bytes)\n"
                        "Status: %s",
                        ota_state_to_string(progress->state),
                        progress->percentage,
                        progress->bytes_written,
                        progress->total_size,
                        progress->status_message);
    }
    else if (strcmp(command, "debug threads") == 0)
    {
        return snprintf(response, response_size,
                        "Active Threads:\n"
                        "• main (priority 0)\n"
                        "• web_server (priority 7)\n"
                        "• ota_manager (priority 6)\n"
                        "• settings (priority 7)\n"
                        "• shell_monitor (priority 8)");
    }

    return snprintf(response, response_size,
                    "Unknown command: %s\nType 'help' for available commands",
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
        LOG_INF("WiFi connected successfully");
        wifi_connected = true;

        // Get IP address when connected
        k_work_schedule(&ip_work, K_SECONDS(2));
        break;

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

    if (progress->state == OTA_STATE_IN_PROGRESS && last_percentage == 255)
    {
        LOG_INF("OTA update started...");
    }

    if (progress->percentage != last_percentage)
    {
        LOG_INF("OTA: %s (%d%%)", progress->status_message, progress->percentage);
        last_percentage = progress->percentage;
    }

    if (progress->state == OTA_STATE_ERROR)
    {
        LOG_ERR("OTA Error: %s", progress->status_message);
    }
    else if (progress->state == OTA_STATE_COMPLETE)
    {
        LOG_INF("✅ OTA Complete - reboot to apply new firmware");
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
                LOG_INF("=== AkiraOS v1.0.0 Test ===");
                ili9341_draw_text(10, 30, "=== AkiraOS v1.0.0 ===", BLACK_COLOR, FONT_7X10);
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

    while (1)
    {
        LOG_INF("... AkiraOS main loop running ...");
        k_sleep(K_SECONDS(30));
    }

    return 0;
}
