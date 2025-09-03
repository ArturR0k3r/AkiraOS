#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include "drivers/display_ili9341.h"
#include "drivers/user_led.h"
#include "drivers/buttons.h"

LOG_MODULE_REGISTER(akira_main, LOG_LEVEL_DBG);

// Simpler, robust test callback for button events
void button_test_callback(int button_id, button_event_t event)
{
    static const char *btn_names[BUTTON_COUNT] = {
        "ON/OFF", "SETTINGS", "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y"};
    static const char *evt_names[] = {
        "NONE", "SINGLE_CLICK", "DOUBLE_CLICK", "TRIPLE_CLICK", "LONG_PRESS"};
    if (button_id < 0 || button_id >= BUTTON_COUNT || event == BUTTON_EVENT_NONE)
        return;
    LOG_INF("Button %s: %s", btn_names[button_id], evt_names[event]);

    if (button_id == BUTTON_ID_A && event == BUTTON_EVENT_SINGLE_CLICK)
    {
        user_led_toggle();
    }
}

int main(void)
{
    LOG_INF("=== Akira Full Hardware Test ===\n");

    int ret;
    const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi2));
    struct spi_config spi_cfg = {0}; // Initialize to zero

    if (!device_is_ready(gpio_dev))
    {
        LOG_ERR("GPIO device not ready!\n");
        return -ENODEV;
    }
    if (!device_is_ready(spi_dev))
    {
        LOG_ERR("SPI device not ready!\n");
        return -ENODEV;
    }

    // Configure GPIO pins first
    ret = gpio_pin_configure(gpio_dev, ILI9341_CS_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure CS pin: %d\n", ret);
        return ret;
    }

    ret = gpio_pin_configure(gpio_dev, ILI9341_DC_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        printk("Failed to configure DC pin: %d\n", ret);
        return ret;
    }

    ret = gpio_pin_configure(gpio_dev, ILI9341_RESET_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        printk("Failed to configure RESET pin: %d\n", ret);
        return ret;
    }

    ret = gpio_pin_configure(gpio_dev, ILI9341_BL_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        printk("Failed to configure backlight pin: %d\n", ret);
        return ret;
    }

    // Set initial pin states
    gpio_pin_set(gpio_dev, ILI9341_CS_PIN, 1); // CS high (inactive)
    gpio_pin_set(gpio_dev, ILI9341_DC_PIN, 0); // DC low
    gpio_pin_set(gpio_dev, ILI9341_BL_PIN, 1); // Backlight on

    // Hardware reset sequence
    printk("Performing hardware reset...\n");
    gpio_pin_set(gpio_dev, ILI9341_RESET_PIN, 1);
    k_msleep(10);
    gpio_pin_set(gpio_dev, ILI9341_RESET_PIN, 0);
    k_msleep(10);
    gpio_pin_set(gpio_dev, ILI9341_RESET_PIN, 1);
    k_msleep(120);

    // Configure SPI - Start with Mode 0
    spi_cfg.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER;
    spi_cfg.frequency = 10000000; // 48MHZ
    spi_cfg.slave = 0;            // Use slave 0

    printk("spi_cfg: freq=%u, op=0x%08x, slave=%d\n",
           spi_cfg.frequency, spi_cfg.operation, spi_cfg.slave);

    // Send a simple command (Software Reset)
    gpio_pin_set(gpio_dev, ILI9341_CS_PIN, 0); // CS low
    gpio_pin_set(gpio_dev, ILI9341_DC_PIN, 0); // DC low for command
    k_usleep(1);

    uint8_t reset_cmd = 0x01; // Software reset command
    struct spi_buf tx_buf = {.buf = &reset_cmd, .len = 1};
    struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};

    ret = spi_write(spi_dev, &spi_cfg, &tx_bufs);

    k_usleep(1);
    gpio_pin_set(gpio_dev, ILI9341_CS_PIN, 1); // CS high

    if (ret < 0)
    {
        LOG_ERR("SPI write failed: %d\n", ret);
        return ret;
    }

    k_msleep(150); // Wait for reset to complete

    // Initialize the display
    ret = ili9341_init(spi_dev, gpio_dev, &spi_cfg);
    if (ret < 0)
    {
        LOG_ERR("Display initialization failed: %d\n", ret);
        return ret;
    }

    if (!device_is_ready(btn_up.port))
    {
        LOG_ERR("GPIO device not ready");
    }
    ret = user_led_init();
    if (ret < 0)
    {
        LOG_ERR("User LED initialization failed: %d\n", ret);
    }
    user_led_on();

    ili9341_fill_color(WHITE_COLOR);
    LOG_INF("=== Akira Full Hardware Test ===\n");
    ili9341_draw_text(50, 30, "=== Akira Full Hardware Test ===", BLACK_COLOR, FONT_7X10);
    LOG_INF("=== AkiraOS v1.0.0 ===");
    ili9341_draw_text(50, 50, "=== AkiraOS v1.0.0 ===", BLACK_COLOR, FONT_7X10);
    LOG_INF("Cyberpunk Gaming Console");
    ili9341_draw_text(50, 70, "Cyberpunk Gaming Console", BLACK_COLOR, FONT_7X10);
    LOG_INF("Hardware: Akira Basic ESP32");
    ili9341_draw_text(50, 90, "Hardware: Akira Basic ESP32", BLACK_COLOR, FONT_7X10);
    LOG_INF("Build: %s %s", __DATE__, __TIME__);
    char build_info[50];
    snprintf(build_info, sizeof(build_info), "Build: %s %s", __DATE__, __TIME__);
    ili9341_draw_text(50, 110, build_info, BLACK_COLOR, FONT_7X10);

    while (1)
    {
        k_sleep(K_MSEC(1000));
    }

    return 0;
}