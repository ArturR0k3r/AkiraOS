/**
 * @file st7789.c
 * @brief ST7789 TFT Display Driver Implementation
 *
 * High-performance SPI driver for ST7789V LCD controller.
 * Supports 240x240 and 240x320 displays with DMA transfers.
 */

#include "st7789.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(st7789, CONFIG_DISPLAY_LOG_LEVEL);

/* ST7789 Commands */
#define ST7789_NOP 0x00
#define ST7789_SWRESET 0x01
#define ST7789_RDDID 0x04
#define ST7789_SLPIN 0x10
#define ST7789_SLPOUT 0x11
#define ST7789_PTLON 0x12
#define ST7789_NORON 0x13
#define ST7789_INVOFF 0x20
#define ST7789_INVON 0x21
#define ST7789_DISPOFF 0x28
#define ST7789_DISPON 0x29
#define ST7789_CASET 0x2A
#define ST7789_RASET 0x2B
#define ST7789_RAMWR 0x2C
#define ST7789_RAMRD 0x2E
#define ST7789_PTLAR 0x30
#define ST7789_TEOFF 0x34
#define ST7789_TEON 0x35
#define ST7789_MADCTL 0x36
#define ST7789_COLMOD 0x3A
#define ST7789_VSCSAD 0x37
#define ST7789_WRDISBV 0x51
#define ST7789_WRCTRLD 0x53

/* MADCTL bits */
#define MADCTL_MY 0x80  // Row address order
#define MADCTL_MX 0x40  // Column address order
#define MADCTL_MV 0x20  // Row/Column exchange
#define MADCTL_ML 0x10  // Vertical refresh order
#define MADCTL_RGB 0x00 // RGB order
#define MADCTL_BGR 0x08 // BGR order

/* Driver state */
static struct
{
    const struct device *spi_dev;
    struct gpio_dt_spec dc_gpio;
    struct gpio_dt_spec rst_gpio;
    struct gpio_dt_spec bl_gpio;
    struct spi_config spi_cfg;
    uint16_t width;
    uint16_t height;
    uint16_t x_offset;
    uint16_t y_offset;
    st7789_rotation_t rotation;
    bool initialized;
} st7789_state;

/**
 * @brief Write command
 */
static int st7789_write_cmd(uint8_t cmd)
{
    // TODO: Implement command write
    // 1. Set DC low (command mode)
    // 2. SPI transmit command byte
    // 3. Handle errors

    (void)cmd;
    LOG_DBG("CMD: 0x%02X", cmd);
    return 0;
}

/**
 * @brief Write data byte
 */
static int st7789_write_data(uint8_t data)
{
    // TODO: Implement data write
    // 1. Set DC high (data mode)
    // 2. SPI transmit data byte
    // 3. Handle errors

    (void)data;
    return 0;
}

/**
 * @brief Write data buffer
 */
static int st7789_write_data_buf(const uint8_t *data, size_t len)
{
    // TODO: Implement bulk data write
    // 1. Set DC high (data mode)
    // 2. SPI transmit buffer
    // 3. Use DMA for large transfers
    // 4. Handle errors

    (void)data;
    (void)len;
    return 0;
}

/**
 * @brief Hardware reset
 */
static void st7789_hw_reset(void)
{
    // TODO: Implement hardware reset
    // 1. Pull RST low
    // 2. Wait 10ms
    // 3. Pull RST high
    // 4. Wait 120ms for display recovery

    LOG_DBG("Hardware reset");
}

/**
 * @brief Initialize display registers
 */
static int st7789_init_regs(const struct st7789_config *config)
{
    // TODO: Implement register initialization
    // 1. Software reset
    // 2. Sleep out
    // 3. Set color format (COLMOD)
    // 4. Set MADCTL for rotation
    // 5. Set column/row address
    // 6. Inversion control
    // 7. Display on

    (void)config;
    LOG_WRN("st7789_init_regs not implemented");
    return 0;
}

int st7789_init(const struct st7789_config *config)
{
    if (!config || !config->spi_dev)
    {
        return -EINVAL;
    }

    LOG_INF("Initializing ST7789 display");
    LOG_INF("  Resolution: %dx%d", config->width, config->height);
    LOG_INF("  Offset: %d, %d", config->x_offset, config->y_offset);
    LOG_INF("  Rotation: %d", config->rotation);

    // TODO: Full initialization
    // 1. Verify SPI device ready
    // 2. Configure GPIOs (DC, RST, BL)
    // 3. Configure SPI (Mode 0, 8-bit, ~40MHz)
    // 4. Hardware reset
    // 5. Initialize registers
    // 6. Set rotation
    // 7. Turn on backlight
    // 8. Clear display

    st7789_state.spi_dev = config->spi_dev;
    st7789_state.dc_gpio = config->dc_gpio;
    st7789_state.rst_gpio = config->rst_gpio;
    st7789_state.bl_gpio = config->bl_gpio;
    st7789_state.width = config->width;
    st7789_state.height = config->height;
    st7789_state.x_offset = config->x_offset;
    st7789_state.y_offset = config->y_offset;
    st7789_state.rotation = config->rotation;

    /* Configure SPI */
    st7789_state.spi_cfg.frequency = 40000000; // 40MHz
    st7789_state.spi_cfg.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8);

    st7789_hw_reset();
    st7789_init_regs(config);

    st7789_state.initialized = true;

    LOG_INF("ST7789 initialized successfully");
    return 0;
}

int st7789_set_rotation(st7789_rotation_t rotation)
{
    if (!st7789_state.initialized)
    {
        return -ENODEV;
    }

    // TODO: Implement rotation
    // - Write MADCTL register with appropriate bits
    // - Update width/height if 90° or 270°

    uint8_t madctl;

    switch (rotation)
    {
    case ST7789_ROTATION_0:
        madctl = MADCTL_MX | MADCTL_MY | MADCTL_RGB;
        break;
    case ST7789_ROTATION_90:
        madctl = MADCTL_MY | MADCTL_MV | MADCTL_RGB;
        break;
    case ST7789_ROTATION_180:
        madctl = MADCTL_RGB;
        break;
    case ST7789_ROTATION_270:
        madctl = MADCTL_MX | MADCTL_MV | MADCTL_RGB;
        break;
    default:
        return -EINVAL;
    }

    st7789_state.rotation = rotation;

    LOG_WRN("st7789_set_rotation: Stub (MADCTL=0x%02X)", madctl);
    return 0;
}

int st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    if (!st7789_state.initialized)
    {
        return -ENODEV;
    }

    // TODO: Implement window setting
    // 1. Add offsets to coordinates
    // 2. Write CASET (column address set)
    // 3. Write RASET (row address set)
    // 4. Write RAMWR to prepare for data

    LOG_DBG("Window: (%d,%d) - (%d,%d)", x0, y0, x1, y1);
    return 0;
}

int st7789_write_pixels(const uint16_t *data, size_t len)
{
    if (!st7789_state.initialized)
    {
        return -ENODEV;
    }

    if (!data || len == 0)
    {
        return -EINVAL;
    }

    // TODO: Implement pixel write
    // 1. Convert to big-endian if needed
    // 2. Write via SPI DMA
    // 3. Handle errors

    LOG_DBG("Write %zu pixels", len);
    return 0;
}

int st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (!st7789_state.initialized)
    {
        return -ENODEV;
    }

    // TODO: Implement rectangle fill
    // 1. Set window to rectangle
    // 2. Write color value (w * h) times
    // 3. Use DMA for efficiency

    LOG_DBG("Fill rect: (%d,%d) %dx%d color=0x%04X", x, y, w, h, color);

    st7789_set_window(x, y, x + w - 1, y + h - 1);

    /* Fill with color */
    // TODO: Optimize with DMA and buffer

    return 0;
}

int st7789_clear(uint16_t color)
{
    return st7789_fill_rect(0, 0, st7789_state.width, st7789_state.height, color);
}

int st7789_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (!st7789_state.initialized)
    {
        return -ENODEV;
    }

    if (x >= st7789_state.width || y >= st7789_state.height)
    {
        return -EINVAL;
    }

    st7789_set_window(x, y, x, y);
    return st7789_write_pixels(&color, 1);
}

int st7789_draw_bitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const uint16_t *bitmap)
{
    if (!st7789_state.initialized)
    {
        return -ENODEV;
    }

    if (!bitmap)
    {
        return -EINVAL;
    }

    st7789_set_window(x, y, x + w - 1, y + h - 1);
    return st7789_write_pixels(bitmap, (size_t)w * h);
}

int st7789_set_backlight(uint8_t level)
{
    if (!st7789_state.initialized)
    {
        return -ENODEV;
    }

    // TODO: Implement backlight control
    // - If PWM available, set duty cycle
    // - Otherwise, simple on/off

    LOG_INF("Backlight: %d%%", level);
    return 0;
}

int st7789_display_on(bool on)
{
    if (!st7789_state.initialized)
    {
        return -ENODEV;
    }

    // TODO: Send DISPON/DISPOFF command

    LOG_INF("Display %s", on ? "ON" : "OFF");
    return st7789_write_cmd(on ? ST7789_DISPON : ST7789_DISPOFF);
}

int st7789_sleep(void)
{
    if (!st7789_state.initialized)
    {
        return -ENODEV;
    }

    // TODO: Enter sleep mode
    // 1. Display off
    // 2. SLPIN command
    // 3. Backlight off

    LOG_INF("Entering sleep mode");
    return st7789_write_cmd(ST7789_SLPIN);
}

int st7789_wake(void)
{
    if (!st7789_state.initialized)
    {
        return -ENODEV;
    }

    // TODO: Exit sleep mode
    // 1. SLPOUT command
    // 2. Wait 120ms
    // 3. Display on
    // 4. Backlight on

    LOG_INF("Waking from sleep");
    return st7789_write_cmd(ST7789_SLPOUT);
}

int st7789_invert(bool invert)
{
    if (!st7789_state.initialized)
    {
        return -ENODEV;
    }

    return st7789_write_cmd(invert ? ST7789_INVON : ST7789_INVOFF);
}

uint16_t st7789_get_width(void)
{
    if (st7789_state.rotation == ST7789_ROTATION_90 ||
        st7789_state.rotation == ST7789_ROTATION_270)
    {
        return st7789_state.height;
    }
    return st7789_state.width;
}

uint16_t st7789_get_height(void)
{
    if (st7789_state.rotation == ST7789_ROTATION_90 ||
        st7789_state.rotation == ST7789_ROTATION_270)
    {
        return st7789_state.width;
    }
    return st7789_state.height;
}
