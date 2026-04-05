/**
 * @file display_hal.c
 * @brief Hardware Display Abstraction Layer for Zephyr Display API
 *
 * Integrates Zephyr's display subsystem with Akira's framebuffer-based
 * display API. Provides a translation layer between the hardware-agnostic
 * akira_display_* API and the physical display hardware.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "../platform_hal.h"

LOG_MODULE_REGISTER(display_hal, LOG_LEVEL_INF);

/* Display device handle */
static const struct device *display_dev = NULL;

/* Display capabilities */
static struct display_capabilities display_caps = {0};

/* Backlight GPIO - GPIO3 on ESP32-S3 */
#define BACKLIGHT_GPIO_NODE DT_NODELABEL(gpio0)
#define BACKLIGHT_GPIO_PIN  3

static const struct device *backlight_gpio_dev = NULL;

/* Display reset is handled by the ST7789V driver via device tree reset-gpios.
 * Do not manually control GPIO15 (RESET) - driver manages hardware reset timing. */

/**
 * @brief Initialize the hardware display
 * @return 0 on success, negative errno on error
 */
int akira_display_hal_init(void)
{
#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_display))

    display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

    if (!device_is_ready(display_dev)) {
        LOG_ERR("Display device not ready");
        display_dev = NULL;  /* clear so get_capabilities returns -ENODEV */
        return -ENODEV;
    }

    /* Enable backlight */
    backlight_gpio_dev = DEVICE_DT_GET(BACKLIGHT_GPIO_NODE);
    if (device_is_ready(backlight_gpio_dev)) {
        gpio_pin_configure(backlight_gpio_dev, BACKLIGHT_GPIO_PIN, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set(backlight_gpio_dev, BACKLIGHT_GPIO_PIN, 1);
    }

    /* Get display capabilities and store them */
    display_get_capabilities(display_dev, &display_caps);

    LOG_INF("Display ready: %dx%d", display_caps.x_resolution, display_caps.y_resolution);

    /* Pre-clear the display GRAM before turning the display on.
     * The framebuffer is zero-initialised (BSS / PSRAM ext_ram.bss), so this
     * writes an all-black frame to GRAM and prevents a flash of GRAM garbage
     * (white noise / previous session content) that would otherwise be visible
     * between display_blanking_off() and the first akira_display_flush() call
     * from main.c. */
    akira_display_hal_flush();

    /* Enable display output */
    int ret = display_blanking_off(display_dev);
    if (ret < 0) {
        LOG_ERR("Failed to enable display: %d", ret);
        return ret;
    }

    return 0;

#else
    LOG_WRN("No display device configured in device tree");
    return -ENOTSUP;
#endif
}

/**
 * @brief Flush framebuffer to physical display
 *
 * Transfers the contents of the Akira framebuffer to the physical display
 * hardware using Zephyr's display_write() API.
 */
void akira_display_hal_flush(void)
{
#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_display))
    if (display_dev == NULL) {
        LOG_ERR("Display not initialized");
        return;
    }

    uint16_t *fb = akira_framebuffer_get();
    if (fb == NULL) {
        LOG_ERR("Framebuffer is NULL");
        return;
    }

    struct display_buffer_descriptor desc = {
        .buf_size = display_caps.x_resolution * display_caps.y_resolution * 2,
        .width    = display_caps.x_resolution,
        .height   = display_caps.y_resolution,
        .pitch    = display_caps.x_resolution,
    };

    int ret = display_write(display_dev, 0, 0, &desc, fb);
    if (ret < 0) {
        LOG_ERR("Display write failed: %d", ret);
    }
#endif
}

/**
 * @brief Get display capabilities
 * @param caps Pointer to capabilities structure to fill
 * @return 0 on success, negative errno on error
 */
int akira_display_hal_get_capabilities(struct display_capabilities *caps)
{
    if (caps == NULL) {
        return -EINVAL;
    }

#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_display))
    if (display_dev == NULL) {
        return -ENODEV;
    }

    *caps = display_caps;
    return 0;
#else
    return -ENOTSUP;
#endif
}

/**
 * @brief Set display backlight brightness (if supported)
 * @param brightness Brightness level (0-100)
 */
void akira_display_hal_set_brightness(uint8_t brightness)
{
    if (backlight_gpio_dev && device_is_ready(backlight_gpio_dev)) {
        gpio_pin_set(backlight_gpio_dev, BACKLIGHT_GPIO_PIN, (brightness > 0) ? 1 : 0);
    }
}

/**
 * @brief Set display rotation
 * ...
 */
int akira_display_hal_set_rotation(uint8_t rotation)
{
#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_display))
    if (display_dev == NULL) {
        LOG_ERR("Display not initialized");
        return -ENODEV;
    }

    /* ST7789V MADCTL register values with BGR bit (0x08)
     * MY=bit7, MX=bit6, MV=bit5, ML=bit4, BGR=bit3 */
    uint8_t madctl_values[4] = {
        0x08,  /* 0°:   Portrait - BGR only */
        0x68,  /* 90°:  Landscape - MX + MV + BGR */
        0xC8,  /* 180°: Portrait inverted - MY + MX + BGR */
        0xA8   /* 270°: Landscape inverted - MY + MV + BGR */
    };

    if (rotation > 3) {
        return -EINVAL;
    }

    /* Use Zephyr display API to send custom command if supported */
    /* For ST7789V: command 0x36 (MADCTL) with data byte */
    
    /* Note: Zephyr's display API doesn't have a standard rotation function yet,
     * so we log the request but cannot actually change it without modifying
     * the device tree mdac property and rebuilding. */
    LOG_INF("Display rotation requested: %d° (MADCTL=0x%02X)", rotation * 90, madctl_values[rotation]);
    LOG_WRN("Runtime rotation not yet implemented - change 'mdac' in device tree");
    
    return -ENOTSUP;
#else
    return -ENOTSUP;
#endif
}

/**
 * @brief Enable or disable display blanking (screen sleep).
 *
 * When blanked the panel backlight and pixel output are disabled.
 * The framebuffer contents are preserved — call akira_display_flush()
 * after akira_display_hal_set_blank(false) to restore the image.
 *
 * @param blank true = screen off, false = screen on.
 */
void akira_display_hal_set_blank(bool blank)
{
#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_display))
    if (!display_dev) return;
    if (blank) {
        /* Backlight off first, then panel blank */
        if (backlight_gpio_dev && device_is_ready(backlight_gpio_dev)) {
            gpio_pin_set(backlight_gpio_dev, BACKLIGHT_GPIO_PIN, 0);
        }
        display_blanking_on(display_dev);
    } else {
        display_blanking_off(display_dev);
        if (backlight_gpio_dev && device_is_ready(backlight_gpio_dev)) {
            gpio_pin_set(backlight_gpio_dev, BACKLIGHT_GPIO_PIN, 1);
        }
    }
#endif
}

/**
 * @brief Write a packed RGB565 buffer directly to the display hardware.
 *
 * Unlike akira_display_hal_flush() this bypasses the OS framebuffer entirely.
 * The caller supplies a buffer with pitch == w (no row stride), allowing the
 * display controller to DMA exactly w*h pixels in a single SPI window.
 *
 * Use this for full-screen game renderers (e.g. NES emulator) where the
 * game maintains its own pixel buffer and writes to a sub-rectangle of the
 * screen every frame, saving both the fb-copy and the full-screen SPI flush.
 *
 * @param x,y   Top-left corner on the display (in pixels)
 * @param w,h   Width and height (pixels)
 * @param data  Packed RGB565 data (w*h*2 bytes, pitch=w, no stride)
 * @return 0 on success, negative errno on error
 */
int akira_display_hal_write_raw(int x, int y, int w, int h, const uint16_t *data)
{
#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_display))
    if (display_dev == NULL || data == NULL || w <= 0 || h <= 0)
        return -EINVAL;

    struct display_buffer_descriptor desc = {
        .buf_size = (uint32_t)((uint32_t)w * (uint32_t)h * 2U),
        .width    = (uint16_t)w,
        .height   = (uint16_t)h,
        .pitch    = (uint16_t)w,   /* packed — stride == width */
    };

    return display_write(display_dev,
                         (uint16_t)x, (uint16_t)y, &desc, data);
#else
    return -ENOTSUP;
#endif
}