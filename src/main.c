#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include "drivers/display_ili9341.h"

LOG_MODULE_REGISTER(akira_main, LOG_LEVEL_INF);

void draw_startup_screen(void)
{
    ili9341_fill_color(BLACK_COLOR);

    // Draw ASCII logo line by line
    const char *logo[] = {
        "AKIRA-OS",
        "",
        "Cyberpunk Console",
        "WASM, Zephyr OS",
        "",
        "Press any button..."};

    int y = 10;
    for (int i = 0; i < sizeof(logo) / sizeof(logo[0]); i++)
    {
        ili9341_draw_text(10, y, logo[i], CYAN_COLOR, FONT_7X10);
        y += 16;
    }
}

void draw_screensaver(void)
{
    for (int frame = 0; frame < 100; frame++)
    {
        ili9341_fill_color((frame % 2) ? MAGENTA_COLOR : CYAN_COLOR);
        ili9341_draw_text(10, 10, "Welcome to Akira Console!", CYAN_COLOR, FONT_7X10);
        k_sleep(K_SECONDS(2));
        ili9341_crt_screensaver();
    }
}

int main(void)
{
    LOG_INF("=== Akira Display Test ===\n");

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

    ili9341_fill_color(WHITE_COLOR);
    LOG_INF("=== AkiraOS v1.0.0 ===");
    ili9341_draw_text(10, 30, "=== AkiraOS v1.0.0 ===", BLACK_COLOR, FONT_7X10);
    LOG_INF("Cyberpunk Gaming Console");
    ili9341_draw_text(10, 50, "Cyberpunk Gaming Console", BLACK_COLOR, FONT_7X10);
    LOG_INF("Hardware: Akira Basic ESP32");
    ili9341_draw_text(10, 70, "Hardware: Akira Basic ESP32", BLACK_COLOR, FONT_7X10);
    LOG_INF("Build: %s %s", __DATE__, __TIME__);

    // Main loop
    while (1)
    {
        k_sleep(K_SECONDS(1));
    }

    return 0;
}