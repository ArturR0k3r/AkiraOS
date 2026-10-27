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
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "../platform_hal.h"
#include "../lib/mem_helper.h"

LOG_MODULE_REGISTER(display_hal, LOG_LEVEL_INF);

/* Conversion buffer for displays whose native format differs from RGB565.
 * Allocated at init time based on the actual display capabilities so that
 * any display (monochrome, colour, any resolution) is handled correctly.
 * Placed in PSRAM when available (akira_malloc_buffer prefers PSRAM). */
#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_display))
static void *conv_buf; /* NULL if no conversion needed */
static size_t conv_buf_size;
#endif

/* Display device handle */
static const struct device *display_dev = NULL;

/* Display capabilities */
static struct display_capabilities display_caps = {0};

/* Backlight PWM — driven from DT alias "pwm-backlight0" / backlight node.
 * On AkiraConsole Prod: LEDC CH0 on GPIO46 → AP2502 EN. */
#define BACKLIGHT_NODE DT_ALIAS(pwm_backlight0)
#define BL_PWM_NODE DT_CHILD(BACKLIGHT_NODE, bl_pwm)

#if DT_NODE_EXISTS(BACKLIGHT_NODE)
static const struct pwm_dt_spec bl_pwm = PWM_DT_SPEC_GET(BL_PWM_NODE);
#endif

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

    if (!device_is_ready(display_dev))
    {
        LOG_ERR("Display device not ready");
        display_dev = NULL; /* clear so get_capabilities returns -ENODEV */
        return -ENODEV;
    }

    /* Force max backlight at startup. */
#if DT_NODE_EXISTS(BACKLIGHT_NODE)
    if (pwm_is_ready_dt(&bl_pwm))
    {
        akira_display_hal_set_brightness(255);
        LOG_INF("Backlight PWM set to max (255)");
    }
    else
    {
        LOG_WRN("Backlight PWM not ready");
    }
#endif

    /* Get display capabilities and store them */
    display_get_capabilities(display_dev, &display_caps);

    LOG_INF("Display ready: %dx%d fmt=%d",
            display_caps.x_resolution, display_caps.y_resolution,
            display_caps.current_pixel_format);

    /* Allocate conversion buffer for non-RGB565 displays.
     * MONO01 / MONO10: 1-bit per pixel, packed 8 per byte.
     * akira_malloc_buffer() prefers PSRAM so DRAM is not pressured. */
    if (display_caps.current_pixel_format == PIXEL_FORMAT_MONO01 ||
        display_caps.current_pixel_format == PIXEL_FORMAT_MONO10)
    {
        conv_buf_size = ((size_t)display_caps.x_resolution *
                             display_caps.y_resolution +
                         7U) /
                        8U;
        conv_buf = akira_malloc_buffer(conv_buf_size);
        if (conv_buf == NULL)
        {
            LOG_ERR("Failed to alloc %zu B MONO conv buf", conv_buf_size);
            return -ENOMEM;
        }
        memset(conv_buf, 0, conv_buf_size);
        LOG_INF("MONO conv buf: %zu B at %p", conv_buf_size, conv_buf);
    }
    /* No conversion buffer needed for RGB565 — framebuffer is already RGB565 */

    /* Pre-clear the display GRAM before turning the display on.
     * The framebuffer is zero-initialised (BSS / PSRAM ext_ram.bss), so this
     * writes an all-black frame to GRAM and prevents a flash of GRAM garbage
     * (white noise / previous session content) that would otherwise be visible
     * between display_blanking_off() and the first akira_display_flush() call
     * from main.c. */
    akira_display_hal_flush();

    /* Test pattern: write alternating white/black horizontal bands so the
     * display is visually testable immediately after boot.  Overwritten once
     * the first application frame arrives. */
    uint16_t *fb = akira_framebuffer_get();
    if (fb != NULL)
    {
        uint32_t w = display_caps.x_resolution;
        uint32_t h = display_caps.y_resolution;
        for (uint32_t y = 0; y < h; y++)
        {
            uint16_t color = ((y / 20U) & 1U) ? 0xFFFFU : 0x0000U;
            for (uint32_t x = 0; x < w; x++)
            {
                fb[y * w + x] = color;
            }
        }
        akira_display_hal_flush();
        /* Leave test pattern visible briefly; main loop will overwrite it */
    }

    /* Enable display output (ENOTSUP is acceptable: disp_en_gpios not wired,
     * DISP pin is pulled HIGH by R85 so display stays on without software control) */
    int ret = display_blanking_off(display_dev);
    if (ret < 0 && ret != -ENOTSUP)
    {
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
    if (display_dev == NULL)
    {
        LOG_ERR("Display not initialized");
        return;
    }

    uint16_t *fb = akira_framebuffer_get();
    if (fb == NULL)
    {
        LOG_ERR("Framebuffer is NULL");
        return;
    }

    if (display_caps.current_pixel_format == PIXEL_FORMAT_MONO01 ||
        display_caps.current_pixel_format == PIXEL_FORMAT_MONO10)
    {
        /* Convert RGB565 → MONO (1 bit per pixel, LSB-first packing).
         * MONO01: bit=0 black, bit=1 white.
         * MONO10: bit=1 black, bit=0 white (inverted).
         * Any non-zero RGB565 pixel is treated as "bright" → white. */
        if (conv_buf == NULL)
        {
            LOG_ERR("Conv buffer not allocated");
            return;
        }
        bool invert = (display_caps.current_pixel_format == PIXEL_FORMAT_MONO10);
        uint16_t w = display_caps.x_resolution;
        uint16_t h = display_caps.y_resolution;
        uint16_t bytes_per_row = (w + 7U) / 8U;

        for (uint16_t y = 0; y < h; y++)
        {
            const uint16_t *row = &fb[y * w];
            uint8_t *dst = &((uint8_t *)conv_buf)[y * bytes_per_row];
            for (uint16_t bx = 0; bx < bytes_per_row; bx++)
            {
                uint8_t packed = 0;
                uint16_t x_base = bx * 8U;
                for (uint8_t bit = 0; bit < 8U; bit++)
                {
                    uint16_t x = x_base + bit;
                    if (x >= w)
                    {
                        break;
                    }
                    bool white = (row[x] != 0x0000U) ^ invert;
                    if (white)
                    {
                        packed |= (uint8_t)(1U << bit); /* LSB-first */
                    }
                }
                dst[bx] = packed;
            }
        }

        struct display_buffer_descriptor desc = {
            .buf_size = (uint32_t)bytes_per_row * h,
            .width = w,
            .height = h,
            .pitch = w,
        };

        int ret = display_write(display_dev, 0, 0, &desc, conv_buf);
        if (ret < 0)
        {
            LOG_ERR("Display write failed: %d", ret);
        }
    }
    else
    {
        /* RGB565 and other colour formats — send framebuffer directly */
        struct display_buffer_descriptor desc = {
            .buf_size = display_caps.x_resolution * display_caps.y_resolution * 2U,
            .width = display_caps.x_resolution,
            .height = display_caps.y_resolution,
            .pitch = display_caps.x_resolution,
        };

        int ret = display_write(display_dev, 0, 0, &desc, fb);
        if (ret < 0)
        {
            LOG_ERR("Display write failed: %d", ret);
        }
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
    if (caps == NULL)
    {
        return -EINVAL;
    }

#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_display))
    if (display_dev == NULL)
    {
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
 * @param brightness Brightness level (0-255)
 */
void akira_display_hal_set_brightness(uint8_t brightness)
{
#if DT_NODE_EXISTS(BACKLIGHT_NODE)
    if (pwm_is_ready_dt(&bl_pwm))
    {
        uint32_t pulse = (uint32_t)bl_pwm.period * brightness / 255;
        pwm_set_dt(&bl_pwm, bl_pwm.period, pulse);
    }
#endif
}

/**
 * @brief Set display rotation
 * ...
 */
int akira_display_hal_set_rotation(uint8_t rotation)
{
#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_display))
    if (display_dev == NULL)
    {
        LOG_ERR("Display not initialized");
        return -ENODEV;
    }

    /* ST7789V MADCTL register values with BGR bit (0x08)
     * MY=bit7, MX=bit6, MV=bit5, ML=bit4, BGR=bit3 */
    uint8_t madctl_values[4] = {
        0x08, /* 0°:   Portrait - BGR only */
        0x68, /* 90°:  Landscape - MX + MV + BGR */
        0xC8, /* 180°: Portrait inverted - MY + MX + BGR */
        0xA8  /* 270°: Landscape inverted - MY + MV + BGR */
    };

    if (rotation > 3)
    {
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
    if (!display_dev)
        return;
    if (blank)
    {
        /* Backlight off first, then panel blank */
#if DT_NODE_EXISTS(BACKLIGHT_NODE)
        if (pwm_is_ready_dt(&bl_pwm))
        {
            pwm_set_dt(&bl_pwm, bl_pwm.period, 0); /* 0% duty = off */
        }
#endif
        display_blanking_on(display_dev);
    }
    else
    {
        display_blanking_off(display_dev);
#if DT_NODE_EXISTS(BACKLIGHT_NODE)
        if (pwm_is_ready_dt(&bl_pwm))
        {
            pwm_set_dt(&bl_pwm, bl_pwm.period, bl_pwm.period); /* 100% */
        }
#endif
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

    /* Reject anything that does not fit entirely on the panel.
     *
     * x/y/w/h are cast to uint16_t below, so without this an out-of-range value
     * silently wraps (x = -1 becomes 65535) and programs a nonsense column/page
     * window into the display controller.  Unlike the framebuffer primitives we
     * reject rather than clip: this path bypasses the framebuffer to hand the
     * controller exactly w*h packed pixels in one SPI window transaction, and
     * clipping would break that 1:1 correspondence with the caller's buffer.
     *
     * Coordinates arrive straight from WASM apps as attacker-controlled int32_t
     * (akira_native_display_raw_write), so the full range must be handled. */
    if (x < 0 || y < 0 ||
        (int64_t)x + w > (int64_t)display_caps.x_resolution ||
        (int64_t)y + h > (int64_t)display_caps.y_resolution)
        return -EINVAL;

    struct display_buffer_descriptor desc = {
        .buf_size = (uint32_t)((uint32_t)w * (uint32_t)h * 2U),
        .width = (uint16_t)w,
        .height = (uint16_t)h,
        .pitch = (uint16_t)w, /* packed — stride == width */
    };

    return display_write(display_dev,
                         (uint16_t)x, (uint16_t)y, &desc, data);
#else
    return -ENOTSUP;
#endif
}