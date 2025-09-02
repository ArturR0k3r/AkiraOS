#include "display_ili9341.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/__assert.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(ili9341, LOG_LEVEL_DBG);

static const struct device *spi_dev_local;
static const struct device *gpio_dev_local;
static struct spi_config *spi_cfg_local;

static int ili9341_send_cmd(uint8_t cmd)
{
    if (!spi_dev_local || !spi_cfg_local || !gpio_dev_local)
    {
        LOG_ERR("NULL pointer in send_cmd");
        return -EINVAL;
    }

    struct spi_buf tx_buf = {.buf = &cmd, .len = 1};
    struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};

    // Set DC low for command
    gpio_pin_set(gpio_dev_local, DC_GPIO_PIN, 0);

    // CS low to start transaction
    gpio_pin_set(gpio_dev_local, ILI9341_CS_PIN, 0);
    k_usleep(1);

    int ret = spi_write(spi_dev_local, spi_cfg_local, &tx_bufs);

    k_usleep(1);
    // CS high to end transaction
    gpio_pin_set(gpio_dev_local, ILI9341_CS_PIN, 1);

    if (ret < 0)
    {
        LOG_ERR("SPI CMD write failed: %d", ret);
    }
    else
    {
        LOG_DBG("SPI CMD: 0x%02X", cmd);
    }

    return ret;
}

static int ili9341_send_data(uint8_t *data, size_t len)
{
    if (!spi_dev_local || !spi_cfg_local || !gpio_dev_local || !data)
    {
        LOG_ERR("NULL pointer in send_data");
        return -EINVAL;
    }

    struct spi_buf tx_buf = {.buf = data, .len = len};
    struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};

    // Set DC high for data
    gpio_pin_set(gpio_dev_local, DC_GPIO_PIN, 1);

    // CS low to start transaction
    gpio_pin_set(gpio_dev_local, ILI9341_CS_PIN, 0);
    k_usleep(1);

    int ret = spi_write(spi_dev_local, spi_cfg_local, &tx_bufs);

    k_usleep(1);
    // CS high to end transaction
    gpio_pin_set(gpio_dev_local, ILI9341_CS_PIN, 1);

    if (ret < 0)
    {
        LOG_ERR("SPI DATA write failed: %d", ret);
    }
    else
    {
        LOG_DBG("SPI DATA: len=%d", (int)len);
    }

    return ret;
}

static int ili9341_send_data_byte(uint8_t data)
{
    return ili9341_send_data(&data, 1);
}

int ili9341_init(const struct device *spi_dev, const struct device *gpio_dev,
                 struct spi_config *spi_cfg)
{
    int ret;

    if (!spi_dev || !gpio_dev || !spi_cfg)
    {
        LOG_ERR("NULL device or config pointer");
        return -EINVAL;
    }

    spi_dev_local = spi_dev;
    gpio_dev_local = gpio_dev;
    spi_cfg_local = spi_cfg;

    LOG_INF("Starting ILI9341 initialization...");

    // Hardware reset (already done in main, but ensure proper timing)
    k_msleep(10);

    // Basic initialization sequence
    ret = ili9341_send_cmd(0x01); // Software reset
    if (ret < 0)
        return ret;
    k_msleep(150);

    ret = ili9341_send_cmd(0x11); // Sleep out
    if (ret < 0)
        return ret;
    k_msleep(120);

    // Power control A
    ret = ili9341_send_cmd(0xCB);
    if (ret < 0)
        return ret;
    uint8_t power_a[] = {0x39, 0x2C, 0x00, 0x34, 0x02};
    ret = ili9341_send_data(power_a, sizeof(power_a));
    if (ret < 0)
        return ret;

    // Power control B
    ret = ili9341_send_cmd(0xCF);
    if (ret < 0)
        return ret;
    uint8_t power_b[] = {0x00, 0x83, 0x30};
    ret = ili9341_send_data(power_b, sizeof(power_b));
    if (ret < 0)
        return ret;

    // Driver timing control A
    ret = ili9341_send_cmd(0xE8);
    if (ret < 0)
        return ret;
    uint8_t timing_a[] = {0x85, 0x01, 0x79};
    ret = ili9341_send_data(timing_a, sizeof(timing_a));
    if (ret < 0)
        return ret;

    // Driver timing control B
    ret = ili9341_send_cmd(0xEA);
    if (ret < 0)
        return ret;
    uint8_t timing_b[] = {0x00, 0x00};
    ret = ili9341_send_data(timing_b, sizeof(timing_b));
    if (ret < 0)
        return ret;

    // Power on sequence control
    ret = ili9341_send_cmd(0xED);
    if (ret < 0)
        return ret;
    uint8_t power_seq[] = {0x64, 0x03, 0x12, 0x81};
    ret = ili9341_send_data(power_seq, sizeof(power_seq));
    if (ret < 0)
        return ret;

    // Pump ratio control
    ret = ili9341_send_cmd(0xF7);
    if (ret < 0)
        return ret;
    uint8_t pump_ratio[] = {0x20};
    ret = ili9341_send_data(pump_ratio, sizeof(pump_ratio));
    if (ret < 0)
        return ret;

    // Power control 1
    ret = ili9341_send_cmd(ILI9341_PWCTR1);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(0x26);
    if (ret < 0)
        return ret;

    // Power control 2
    ret = ili9341_send_cmd(ILI9341_PWCTR2);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(0x11);
    if (ret < 0)
        return ret;

    // VCOM control 1
    ret = ili9341_send_cmd(ILI9341_VMCTR1);
    if (ret < 0)
        return ret;
    uint8_t vcom1[] = {0x35, 0x3E};
    ret = ili9341_send_data(vcom1, sizeof(vcom1));
    if (ret < 0)
        return ret;

    // VCOM control 2
    ret = ili9341_send_cmd(ILI9341_VMCTR2);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(0xBE);
    if (ret < 0)
        return ret;

    // Memory access control
    ret = ili9341_send_cmd(ILI9341_MADCTL);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(0x28);
    if (ret < 0)
        return ret;

    // Pixel format (16-bit color)
    ret = ili9341_send_cmd(ILI9341_COLMOD);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(0x55);
    if (ret < 0)
        return ret;

    // Frame rate control
    ret = ili9341_send_cmd(0xB1);
    if (ret < 0)
        return ret;
    uint8_t frame_rate[] = {0x00, 0x1B};
    ret = ili9341_send_data(frame_rate, sizeof(frame_rate));
    if (ret < 0)
        return ret;

    // Display function control
    ret = ili9341_send_cmd(0xB6);
    if (ret < 0)
        return ret;
    uint8_t display_func[] = {0x0A, 0x82, 0x27, 0x00};
    ret = ili9341_send_data(display_func, sizeof(display_func));
    if (ret < 0)
        return ret;

    // 3Gamma function disable
    ret = ili9341_send_cmd(0xF2);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(0x08);
    if (ret < 0)
        return ret;

    // Gamma curve selected
    ret = ili9341_send_cmd(0x26);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(0x01);
    if (ret < 0)
        return ret;

    // Positive gamma correction
    ret = ili9341_send_cmd(ILI9341_GMCTRP1);
    if (ret < 0)
        return ret;
    uint8_t gamma_p[] = {0x1F, 0x1A, 0x18, 0x0A, 0x0F, 0x06, 0x45, 0x87,
                         0x32, 0x0A, 0x07, 0x02, 0x07, 0x05, 0x00};
    ret = ili9341_send_data(gamma_p, sizeof(gamma_p));
    if (ret < 0)
        return ret;

    // Negative gamma correction
    ret = ili9341_send_cmd(ILI9341_GMCTRN1);
    if (ret < 0)
        return ret;
    uint8_t gamma_n[] = {0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3A, 0x78,
                         0x4D, 0x05, 0x18, 0x0D, 0x38, 0x3A, 0x1F};
    ret = ili9341_send_data(gamma_n, sizeof(gamma_n));
    if (ret < 0)
        return ret;

    // Set column address
    ret = ili9341_send_cmd(ILI9341_CASET);
    if (ret < 0)
        return ret;
    uint8_t col_addr[] = {0x00, 0x00, 0x00, 0xEF};
    ret = ili9341_send_data(col_addr, sizeof(col_addr));
    if (ret < 0)
        return ret;

    // Set page address
    ret = ili9341_send_cmd(ILI9341_PASET);
    if (ret < 0)
        return ret;
    uint8_t page_addr[] = {0x00, 0x00, 0x01, 0x3F};
    ret = ili9341_send_data(page_addr, sizeof(page_addr));
    if (ret < 0)
        return ret;

    // Entry mode set
    ret = ili9341_send_cmd(0xB7);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(0x07);
    if (ret < 0)
        return ret;

    // Display on
    ret = ili9341_send_cmd(ILI9341_DISPON);
    if (ret < 0)
        return ret;
    k_msleep(120);

    LOG_INF("ILI9341 initialization completed successfully");
    return 0;
}

static int ili9341_set_area(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end)
{
    int ret;

    ret = ili9341_send_cmd(ILI9341_CASET);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(x_start >> 8);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(x_start & 0xFF);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(x_end >> 8);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(x_end & 0xFF);
    if (ret < 0)
        return ret;

    ret = ili9341_send_cmd(ILI9341_PASET);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(y_start >> 8);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(y_start & 0xFF);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(y_end >> 8);
    if (ret < 0)
        return ret;
    ret = ili9341_send_data_byte(y_end & 0xFF);
    if (ret < 0)
        return ret;

    return 0;
}

int ili9341_fill_color(uint16_t color)
{
    int ret = ili9341_set_area(0, 0, ILI9341_DISPLAY_WIDTH - 1, ILI9341_DISPLAY_HEIGHT - 1);
    if (ret < 0)
        return ret;

    ret = ili9341_send_cmd(ILI9341_RAMWR);
    if (ret < 0)
        return ret;

    uint8_t color_bytes[2] = {color >> 8, color & 0xFF};

    // Send color data for entire screen
    for (int i = 0; i < ILI9341_DISPLAY_WIDTH * ILI9341_DISPLAY_HEIGHT; i++)
    {
        ret = ili9341_send_data(color_bytes, 2);
        if (ret < 0)
            return ret;
    }

    return 0;
}

// Rest of the functions remain the same...
int ili9341_draw_color_bars(void)
{
    uint16_t colors[] = {WHITE_COLOR, RED_COLOR, GREEN_COLOR, BLUE_COLOR,
                         YELLOW_COLOR, MAGENTA_COLOR, CYAN_COLOR, BLACK_COLOR};
    int bar_height = ILI9341_DISPLAY_HEIGHT / 8;

    for (int i = 0; i < 8; i++)
    {
        int y_start = i * bar_height;
        int y_end = (i == 7) ? ILI9341_DISPLAY_HEIGHT - 1 : (i + 1) * bar_height - 1;

        ili9341_set_area(0, y_start, ILI9341_DISPLAY_WIDTH - 1, y_end);
        ili9341_send_cmd(ILI9341_RAMWR);

        uint8_t color_bytes[2] = {colors[i] >> 8, colors[i] & 0xFF};
        int pixels_in_bar = ILI9341_DISPLAY_WIDTH * (y_end - y_start + 1);

        for (int p = 0; p < pixels_in_bar; p++)
        {
            ili9341_send_data(color_bytes, 2);
        }
    }
    return 0;
}

// Keep the existing font and drawing functions...
static const uint8_t font8x8_basic[96][8] = {
    // Space (32)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // '!' (33)
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},
    // Add rest of font data...
    // (keeping existing font data for brevity)
};

void ili9341_draw_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= ILI9341_DISPLAY_WIDTH || y < 0 || y >= ILI9341_DISPLAY_HEIGHT)
    {
        return; // Out of bounds
    }

    ili9341_set_area(x, y, x, y);
    ili9341_send_cmd(ILI9341_RAMWR);
    uint8_t color_bytes[2] = {color >> 8, color & 0xFF};
    ili9341_send_data(color_bytes, 2);
}

static void ili9341_draw_char(int x, int y, char c, uint16_t color)
{
    if (c < 32 || c > 127)
        return;

    const uint8_t *bitmap = font8x8_basic[c - 32];
    for (int row = 0; row < 8; row++)
    {
        for (int col = 0; col < 8; col++)
        {
            if (bitmap[row] & (1 << col))
            {
                ili9341_draw_pixel(x + col, y + row, color);
            }
        }
    }
}

void ili9341_draw_text(int x, int y, const char *text, uint16_t color)
{
    int cursor_x = x;
    while (*text)
    {
        ili9341_draw_char(cursor_x, y, *text, color);
        cursor_x += 8;
        text++;
    }
}

void ili9341_crt_screensaver(void)
{
    for (int frame = 0; frame < 100; frame++)
    {
        ili9341_fill_color(BLACK_COLOR);

        // Draw random colored horizontal lines
        for (int y = 0; y < ILI9341_DISPLAY_HEIGHT; y += 4)
        {
            uint16_t color = (frame % 3 == 0) ? CYAN_COLOR : (frame % 3 == 1) ? MAGENTA_COLOR
                                                                              : GREEN_COLOR;
            for (int x = 0; x < ILI9341_DISPLAY_WIDTH; x += 2)
            {
                ili9341_draw_pixel(x, y, color);
            }
        }

        // Draw scrolling text
        int scroll_y = (frame * 2) % (ILI9341_DISPLAY_HEIGHT - 8);
        ili9341_draw_text(10, scroll_y, "AKIRA CONSOLE", CYAN_COLOR);

        k_sleep(K_MSEC(100));
    }
}

int ili9341_backlight_init(const struct device *gpio_dev, int pin)
{
    int ret = gpio_pin_configure(gpio_dev, pin, GPIO_OUTPUT_ACTIVE);
    if (ret < 0)
        return ret;

    gpio_pin_set(gpio_dev, pin, 1);
    LOG_INF("Backlight initialized on GPIO %d", pin);
    return 0;
}