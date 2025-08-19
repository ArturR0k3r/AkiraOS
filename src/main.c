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
        " █████╗ ██╗  ██╗██╗██████╗  █████╗        ██████╗ ███████╗  ",
        "██╔══██╗██║ ██╔╝██║██╔══██╗██╔══██╗      ██╔═══██╗██╔════╝  ",
        "███████║█████╔╝ ██║██████╔╝███████║█████╗██║   ██║███████╗  ",
        "██╔══██║██╔═██╗ ██║██╔══██╗██╔══██║╚════╝██║   ██║╚════██║  ",
        "██║  ██║██║  ██╗██║██║  ██║██║  ██║      ╚██████╔╝███████║  ",
        "╚═╝  ╚═╝╚═╝  ╚═╝╚═╝╚═╝  ╚═╝╚═╝  ╚═╝       ╚═════╝ ╚══════╝  ",
        "",
        "🎮 Akira Console - Minimalist Retro-Cyberpunk",
        "WASM, Zephyr OS, CyberSec Tools",
        "",
        "Press any button to start..."};

    int y = 10;
    for (int i = 0; i < sizeof(logo) / sizeof(logo[0]); i++)
    {
        ili9341_draw_text(10, y, logo[i], CYAN_COLOR); // You need to implement ili9341_draw_text
        y += 16;                                       // Advance by font height
    }
}

void draw_screensaver(void)
{
    for (int frame = 0; frame < 100; frame++)
    {
        ili9341_fill_color((frame % 2) ? MAGENTA_COLOR : CYAN_COLOR);
        // Optionally draw random lines or text
        ili9341_draw_text(10, 10, "Welcome to Akira Console!", CYAN_COLOR);
        k_sleep(K_SECONDS(2));
        ili9341_crt_screensaver();
    }
}

int main(void)
{
    int ret;
    printk("=== ESP32 ILI9341 Display Test Starting ===\n");

    const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio_dev))
    {
        printk("ERROR: GPIO device not ready!\n");
        return -ENODEV;
    }
    const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi2));
    if (!device_is_ready(spi_dev))
    {
        printk("ERROR: SPI device not ready!\n");
        return -ENODEV;
    }

    struct spi_config spi_cfg = {
        .operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB, // Set to MODE0 (CPOL=0, CPHA=0)
        .frequency = 10000000,                           // Lowered to 10MHz
        .slave = 0,
    };

    // Backlight
    printk("Initializing backlight on GPIO27...\n");
    ret = ili9341_backlight_init(gpio_dev, 27);
    if (ret < 0)
    {
        printk("Warning: Backlight initialization failed\n");
    }
    else
    {
        printk("Backlight initialized and turned ON.\n");
    }

    // Display init
    printk("Initializing ILI9341 display...\n");
    ret = ili9341_init(spi_dev, gpio_dev, &spi_cfg);
    if (ret < 0)
    {
        printk("ERROR: Failed to initialize display: %d\n", ret);
        return ret;
    }
    printk("Display initialized successfully\n");

    printk("Filling display with WHITE after init...\n");
    ret = ili9341_fill_color(WHITE_COLOR);
    if (ret < 0)
    {
        printk("ERROR: Failed to fill display with white: %d\n", ret);
    }
    else
    {
        printk("Display filled with white.\n");
    }

    draw_startup_screen(); // Show the startup screen

    while (1)
    {
        k_sleep(K_SECONDS(5)); // Idle loop for testing
    }
    return 0;
}