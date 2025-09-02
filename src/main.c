#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include "drivers/display_ili9341.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

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
        ili9341_draw_text(10, y, logo[i], CYAN_COLOR);
        y += 16;
    }
}

void draw_screensaver(void)
{
    for (int frame = 0; frame < 100; frame++)
    {
        ili9341_fill_color((frame % 2) ? MAGENTA_COLOR : CYAN_COLOR);
        ili9341_draw_text(10, 10, "Welcome to Akira Console!", CYAN_COLOR);
        k_sleep(K_SECONDS(2));
        ili9341_crt_screensaver();
    }
}

int main(void)
{
    int ret;
    const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi2));
    struct spi_config spi_cfg = {0}; // Initialize to zero

    printk("=== ESP32 ILI9341 Display Test (Fixed Version) ===\n");

    if (!device_is_ready(gpio_dev))
    {
        printk("ERROR: GPIO device not ready!\n");
        return -ENODEV;
    }
    if (!device_is_ready(spi_dev))
    {
        printk("ERROR: SPI device not ready!\n");
        return -ENODEV;
    }

    // Configure GPIO pins first
    ret = gpio_pin_configure(gpio_dev, ILI9341_CS_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        printk("ERROR: Failed to configure CS pin: %d\n", ret);
        return ret;
    }

    ret = gpio_pin_configure(gpio_dev, ILI9341_DC_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        printk("ERROR: Failed to configure DC pin: %d\n", ret);
        return ret;
    }

    ret = gpio_pin_configure(gpio_dev, ILI9341_RESET_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        printk("ERROR: Failed to configure RESET pin: %d\n", ret);
        return ret;
    }

    ret = gpio_pin_configure(gpio_dev, ILI9341_BL_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
    {
        printk("ERROR: Failed to configure backlight pin: %d\n", ret);
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

    // Configure SPI - Start with Mode 0 instead of Mode 3
    spi_cfg.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER;
    spi_cfg.frequency = 1000000; // Start slow
    spi_cfg.slave = 0;           // Use slave 0 instead of 2
    

    printk("spi_cfg: freq=%u, op=0x%08x, slave=%d\n",
           spi_cfg.frequency, spi_cfg.operation, spi_cfg.slave);

    // Test basic SPI communication
    printk("Testing basic SPI communication...\n");

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
        printk("ERROR: SPI write failed: %d\n", ret);
        return ret;
    }
    else
    {
        printk("SPI write successful!\n");
    }

    k_msleep(150); // Wait for reset to complete

    // Initialize the display
    printk("Initializing ILI9341 display...\n");
    ret = ili9341_init(spi_dev, gpio_dev, &spi_cfg);
    if (ret < 0)
    {
        printk("ERROR: Display initialization failed: %d\n", ret);
        return ret;
    }

    printk("Display initialized successfully!\n");

    // Test display functionality
    printk("Testing display colors...\n");
    ili9341_fill_color(RED_COLOR);
    k_sleep(K_SECONDS(1));

    ili9341_fill_color(GREEN_COLOR);
    k_sleep(K_SECONDS(1));

    ili9341_fill_color(BLUE_COLOR);
    k_sleep(K_SECONDS(1));

    // Draw startup screen
    draw_startup_screen();
    k_sleep(K_SECONDS(3));

    // Run screensaver
    draw_screensaver();

    // Main loop
    while (1)
    {
        k_sleep(K_SECONDS(5));
        printk("Display running...\n");
    }

    return 0;
}