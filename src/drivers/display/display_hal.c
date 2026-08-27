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
#ifdef CONFIG_AKIRA_LP5817
#include "lp5817.h"
#endif
#include "../lib/mem_helper.h"

LOG_MODULE_REGISTER(display_hal, LOG_LEVEL_INF);

/* Conversion buffer for displays whose native format differs from RGB565.
 * Allocated at init time based on the actual display capabilities so that
 * any display (monochrome, colour, any resolution) is handled correctly.
 * Placed in PSRAM when available (akira_malloc_buffer prefers PSRAM). */
#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_display))
static void *conv_buf;   /* NULL if no conversion needed */
static void *shadow_buf; /* Shadow of last-sent MONO frame for dirty tracking */
static size_t conv_buf_size;
static size_t bytes_per_row; /* Cached: (width + 7) / 8 — valid only for MONO */
#endif

/* Display device handle */
static const struct device *display_dev = NULL;

/* Display capabilities */
static struct display_capabilities display_caps = {0};

/* Backlight PWM — driven from DT alias "pwm-backlight0" / backlight node.
 * On AkiraConsole Prod: LEDC CH0 on GPIO46 → AP2502 EN. */
#define BACKLIGHT_NODE DT_ALIAS(pwm_backlight0)
#define BL_PWM_NODE DT_CHILD(BACKLIGHT_NODE, bl_pwm)

#if DT_NODE_EXISTS(BACKLIGHT_NODE) && !defined(CONFIG_AKIRA_LP5817)
static const struct pwm_dt_spec bl_pwm = PWM_DT_SPEC_GET(BL_PWM_NODE);
#endif

/* Display reset is handled by the ST7789V driver via device tree reset-gpios.
 * Do not manually control GPIO15 (RESET) - driver manages hardware reset timing. */

/* Dedicated blit thread, pinned to core0 — the opposite core from WASM app
 * threads (pinned core1, akira_runtime.c) — so app compute for frame N+1
 * overlaps the blit of frame N instead of serializing on one core. */
#define COMPOSITOR_CORE_ID 0

static void compositor_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (;;)
    {
        const uint16_t *fb = akira_framebuffer_wait_present();
        akira_display_hal_flush_buf(fb);
        akira_framebuffer_present_done();
    }
}

K_THREAD_STACK_DEFINE(compositor_stack, CONFIG_AKIRA_DISPLAY_COMPOSITOR_STACK_SIZE);
static struct k_thread compositor_tid;

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
#ifdef CONFIG_AKIRA_LP5817
    /* AkiraConsole Prod: the panel backlight is an LP5817 I2C LED driver
     * (U7), not a PWM GPIO.  It powers up with every register cleared, so
     * without this the panel is completely dark no matter what is drawn. */
    if (akira_lp5817_init() == 0)
    {
        akira_display_hal_set_brightness(255);
        LOG_INF("LP5817 backlight set to max (255)");
    }
    else
    {
        LOG_WRN("LP5817 backlight init failed — panel will stay dark");
    }
#elif DT_NODE_EXISTS(BACKLIGHT_NODE)
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
        bytes_per_row = ((size_t)display_caps.x_resolution + 7U) / 8U;
        conv_buf_size = bytes_per_row * display_caps.y_resolution;
        conv_buf = akira_malloc_buffer(conv_buf_size);
        if (conv_buf == NULL)
        {
            LOG_ERR("Failed to alloc %zu B MONO conv buf", conv_buf_size);
            return -ENOMEM;
        }
        memset(conv_buf, 0, conv_buf_size);

        /* Shadow buffer: copy of the last frame sent to the display.
         * Dirty tracking: only write scanlines that have changed since the
         * previous flush, reducing average SPI transfer volume significantly. */
        shadow_buf = akira_malloc_buffer(conv_buf_size);
        if (shadow_buf != NULL)
        {
            /* Initialise to all-0xFF (guaranteed different from the cleared
             * conv_buf) so the very first flush sends the whole frame. */
            memset(shadow_buf, 0xFF, conv_buf_size);
            LOG_INF("MONO conv+shadow bufs: %zu B each at %p/%p",
                    conv_buf_size, conv_buf, shadow_buf);
        }
        else
        {
            LOG_WRN("Shadow buf alloc failed — dirty tracking disabled");
        }
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

    k_thread_create(&compositor_tid, compositor_stack,
                    K_THREAD_STACK_SIZEOF(compositor_stack),
                    compositor_thread_fn, NULL, NULL, NULL,
                    CONFIG_AKIRA_DISPLAY_COMPOSITOR_PRIORITY,
                    0, K_FOREVER);
    k_thread_cpu_pin(&compositor_tid, COMPOSITOR_CORE_ID);
    k_thread_start(&compositor_tid);

    return 0;

#else
    LOG_WRN("No display device configured in device tree");
    return -ENOTSUP;
#endif
}

/**
 * @brief Flush an explicit framebuffer to physical display hardware
 *
 * Transfers the given buffer to the physical display hardware using
 * Zephyr's display_write() API.
 */
void akira_display_hal_flush_buf(const uint16_t *fb)
{
#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_display))
    int64_t flush_start_ms = k_uptime_get();

    if (display_dev == NULL)
    {
        return;
    }

    if (fb == NULL)
    {
        LOG_ERR("Framebuffer is NULL");
        return;
    }

    const uint16_t w = display_caps.x_resolution;
    const uint16_t h = display_caps.y_resolution;

/* ══════════════════════════════════════════════════════════════════════
 *  MONO01 / MONO10
 * ══════════════════════════════════════════════════════════════════════ */

/* PX_WHITE(p): any non-zero RGB565 pixel → white; 0x0000 → black. */
#define PX_WHITE(p) ((p) != 0U)

    if (display_caps.current_pixel_format == PIXEL_FORMAT_MONO01 ||
        display_caps.current_pixel_format == PIXEL_FORMAT_MONO10)
    {
        if (conv_buf == NULL)
        {
            LOG_ERR("conv_buf not allocated");
            return;
        }

        const bool invert = (display_caps.current_pixel_format == PIXEL_FORMAT_MONO10) ||
                            IS_ENABLED(CONFIG_AKIRA_SHARP_INVERT_COLORS);
        const size_t bpr = bytes_per_row;
        const size_t full_bx = (size_t)w >> 3U;             /* complete 8-px groups   */
        const uint8_t tail_w = (uint8_t)((uint16_t)w & 7U); /* leftover pixels [0..7] */

        uint8_t *restrict mono = (uint8_t *)conv_buf;
        uint8_t *restrict shad = (uint8_t *)shadow_buf; /* may be NULL */

        /* ── 1. RGB565 → packed MONO (LSB = leftmost pixel) ─────────────── */
        for (uint16_t y = 0U; y < h; y++)
        {
            const uint16_t *restrict row = &fb[(size_t)y * w];
            uint8_t *restrict dst = &mono[(size_t)y * bpr];

            for (size_t bx = 0U; bx < full_bx; bx++)
            {
                const uint16_t *px = &row[bx * 8U];
                const uint8_t b = (uint8_t)(((uint8_t)PX_WHITE(px[0]) << 0U) | ((uint8_t)PX_WHITE(px[1]) << 1U) |
                                            ((uint8_t)PX_WHITE(px[2]) << 2U) | ((uint8_t)PX_WHITE(px[3]) << 3U) |
                                            ((uint8_t)PX_WHITE(px[4]) << 4U) | ((uint8_t)PX_WHITE(px[5]) << 5U) |
                                            ((uint8_t)PX_WHITE(px[6]) << 6U) | ((uint8_t)PX_WHITE(px[7]) << 7U));
                dst[bx] = invert ? (uint8_t)(b ^ 0xFFU) : b;
            }

            if (tail_w != 0U)
            {
                /* Partial last byte — unused high bits remain 0 */
                const uint16_t *px = &row[full_bx * 8U];
                uint8_t b = 0U;
                for (uint8_t bit = 0U; bit < tail_w; bit++)
                {
                    b |= (uint8_t)((uint8_t)PX_WHITE(px[bit]) << bit);
                }
                dst[full_bx] = invert ? (uint8_t)(b ^ 0xFFU) : b;
            }
        }

        /* ── 2. Dirty-rect detection → one display_write per frame ─────── */
        if (shad != NULL)
        {
            const uint8_t *restrict nf = mono;
            const uint8_t *restrict of = shad;

            /*
             * Accumulate a bounding rect over all changed bytes at
             * byte-column granularity (8 px per column) — no bit extraction
             * needed when handing the sub-buffer to display_write.
             *
             * One display_write call per frame, covering the minimum dirty
             * bounding box.  Optimal for localised motion (game sprites,
             * animated widgets).  For non-adjacent multi-region changes
             * (e.g. top-left + bottom-right simultaneously), dirty spans
             * transfer fewer raw bytes — switch strategy if that's typical.
             */
            /* Sharp LS0XX requires every display_write to start at x=0
             * with the full panel width.  Partial-column writes are rejected
             * with -EINVAL ("Width not a multiple of 400").
             *
             * Strategy: track dirty *rows* only, then write the full-width
             * row slice [dy0, dy1) in a single call — x=0, width=w always.
             */
            uint16_t dy0 = h;
            uint16_t dy1 = 0U;

            for (uint16_t y = 0U; y < h; y++)
            {
                if (memcmp(&nf[(size_t)y * bpr], &of[(size_t)y * bpr], bpr) != 0)
                {
                    if (y < dy0) { dy0 = y; }
                    dy1 = y + 1U;
                }
            }

            if (dy1 == 0U)
            {
                return; /* frame identical — nothing to send */
            }

            const uint16_t rh = dy1 - dy0;

            /* Full-width write: x=0, width=w (required by Sharp ls0xx driver) */
            struct display_buffer_descriptor desc = {
                .buf_size = (uint32_t)rh * (uint32_t)bpr,
                .width    = w,
                .height   = rh,
                .pitch    = w,
            };
            int ret = display_write(display_dev, 0, dy0, &desc,
                                    &nf[(size_t)dy0 * bpr]);
            if (ret < 0)
            {
                LOG_ERR("display_write(mono y=%u h=%u) -> %d", dy0, rh, ret);
            }

            /* Commit shadow for the rows we just sent */
            memcpy(&shad[(size_t)dy0 * bpr],
                   &mono[(size_t)dy0 * bpr],
                   (size_t)rh * bpr);
        }
        else
        {
            /* No shadow — unconditional full-frame write */
            struct display_buffer_descriptor desc = {
                .buf_size = (uint32_t)conv_buf_size,
                .width = w,
                .height = h,
                .pitch = w,
            };
            int ret = display_write(display_dev, 0, 0, &desc, mono);
            if (ret < 0)
            {
                LOG_ERR("display_write(mono full) -> %d", ret);
            }
        }
    }
    /* ══════════════════════════════════════════════════════════════════════
     *  RGB565 / other colour formats — direct framebuffer passthrough
     * ══════════════════════════════════════════════════════════════════════ */
    else
    {
        struct display_buffer_descriptor desc = {
            .buf_size = (uint32_t)w * (uint32_t)h * 2U,
            .width = w,
            .height = h,
            .pitch = w,
        };
        int ret = display_write(display_dev, 0, 0, &desc, fb);
        if (ret < 0)
        {
            LOG_ERR("display_write(rgb565 full) -> %d", ret);
        }
    }

    LOG_INF("flush: %lld ms", k_uptime_delta(&flush_start_ms));
#endif /* DT_NODE_EXISTS(DT_CHOSEN(zephyr_display)) */
}

/**
 * @brief Flush framebuffer to physical display
 *
 * Convenience wrapper around akira_display_hal_flush_buf() for callers
 * with no double buffer (shell commands, boot test pattern).
 */
void akira_display_hal_flush(void)
{
    akira_display_hal_flush_buf(akira_framebuffer_get());
}

/**
 * @brief Clear framebuffer to black and flush to display.
 *
 * Zeroes the entire RGB565 framebuffer and calls akira_display_hal_flush().
 * Called by the runtime before launching each WASM app so no leftover pixels
 * from the previous app are visible during startup.
 * Does not require callers to include <zephyr/drivers/display.h>.
 */
void akira_display_hal_clear(void)
{
#if DT_NODE_EXISTS(DT_CHOSEN(zephyr_display))
    uint16_t *fb = akira_framebuffer_get();
    if (fb != NULL && display_caps.x_resolution > 0 && display_caps.y_resolution > 0)
    {
        memset(fb, 0,
               (size_t)display_caps.x_resolution *
                   (size_t)display_caps.y_resolution *
                   sizeof(uint16_t));
        akira_display_hal_flush();
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
#ifdef CONFIG_AKIRA_LP5817
    (void)akira_lp5817_set_brightness(brightness);
#elif DT_NODE_EXISTS(BACKLIGHT_NODE)
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
        akira_display_hal_set_brightness(0);
        display_blanking_on(display_dev);
    }
    else
    {
        display_blanking_off(display_dev);
        akira_display_hal_set_brightness(255);
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

    /* MONO displays (e.g. Sharp LS0XX) require full-row writes (width ==
     * panel width) and don't accept partial RGB565 tiles directly.
     * Route through the framebuffer: blit RGB565 data into the correct
     * position, then do a full-frame flush (which handles MONO conversion). */
    if (display_caps.current_pixel_format == PIXEL_FORMAT_MONO01 ||
        display_caps.current_pixel_format == PIXEL_FORMAT_MONO10)
    {
        uint16_t *fb = akira_framebuffer_get();
        if (!fb)
            return -ENOMEM;

        int x1 = MAX(x, 0);
        int y1 = MAX(y, 0);
        int x2 = MIN(x + w, (int)display_caps.x_resolution);
        int y2 = MIN(y + h, (int)display_caps.y_resolution);

        for (int py = y1; py < y2; py++)
        {
            int span = x2 - x1;
            if (span <= 0)
                continue;
            const uint16_t *src = data + (py - y) * w + (x1 - x);
            uint16_t *dst = fb + py * display_caps.x_resolution + x1;
            memcpy(dst, src, (size_t)span * sizeof(uint16_t));
        }

        akira_display_hal_flush();
        return 0;
    }

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