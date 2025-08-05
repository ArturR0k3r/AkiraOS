// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <string.h>


LOG_MODULE_REGISTER(akira_main, LOG_LEVEL_DBG);

int main(void)
{
    LOG_INF("=== AkiraOS v1.0.0 ===");
    LOG_INF("Cyberpunk Gaming Console");
    LOG_INF("Hardware: Akira Basic ESP32");
    LOG_INF("Build: %s %s", __DATE__, __TIME__);

 

    const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

    if (!device_is_ready(display)) {
        LOG_INF("Display device not ready\n");
        return;
    }
    LOG_INF("AkiraOS initialization complete");
    LOG_INF("System ready for hacking...");
    LOG_INF("Display ready\n");

    struct display_capabilities capabilities;
    display_get_capabilities(display, &capabilities);
    uint32_t width = capabilities.x_resolution;
    uint32_t height = capabilities.y_resolution;
    // Print out the retrieved information

    printk("Display Capabilities:\n");
    printk("  X Resolution: %d\n", capabilities.x_resolution);
    printk("  Y Resolution: %d\n", capabilities.y_resolution);
    printk("  Supported Pixel Formats: 0x%x\n", capabilities.supported_pixel_formats);

    // Prepare a full-white frame buffer
    size_t buffer_size = width * height * 3;  // RGB888 = 3 bytes/pixel
    uint8_t fb[256];
    memset(fb, 0xFF, buffer_size); // fill with white

    if (!fb) {
        LOG_INF("Failed to allocate framebuffer\n");
        return -1;
    }
    struct display_buffer_descriptor desc = {
        .buf_size = buffer_size,
        .width = width,
        .height = height,
        .pitch = width,
    };

    display_write(display, 0, 0, &desc, fb);
    LOG_INF("Display filled with white\n");

    /* Main loop */
    bool led_state = false;
    while (1)
    {
        LOG_INF("System heartbeat - LED: %s", led_state ? "ON" : "OFF");
        k_msleep(500);
        bool led_state = !led_state;
    }
    return 0;
}