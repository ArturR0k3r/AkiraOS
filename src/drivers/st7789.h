/**
 * @file st7789.h
 * @brief ST7789 TFT Display Driver
 *
 * ST7789V 240x240/240x320 TFT LCD driver with SPI interface.
 * Used on Akira-Console display module.
 */

#ifndef AKIRA_ST7789_H
#define AKIRA_ST7789_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief ST7789 rotation options
     */
    typedef enum
    {
        ST7789_ROTATION_0 = 0,
        ST7789_ROTATION_90 = 1,
        ST7789_ROTATION_180 = 2,
        ST7789_ROTATION_270 = 3
    } st7789_rotation_t;

    /**
     * @brief ST7789 color format
     */
    typedef enum
    {
        ST7789_COLOR_RGB565 = 0, // 16-bit RGB565
        ST7789_COLOR_RGB666 = 1  // 18-bit RGB666
    } st7789_color_format_t;

    /**
     * @brief ST7789 hardware configuration
     */
    struct st7789_config
    {
        const struct device *spi_dev;
        struct gpio_dt_spec dc_gpio;  // Data/Command
        struct gpio_dt_spec rst_gpio; // Reset
        struct gpio_dt_spec bl_gpio;  // Backlight
        uint16_t width;               // Display width
        uint16_t height;              // Display height
        uint16_t x_offset;            // X offset for smaller displays
        uint16_t y_offset;            // Y offset for smaller displays
        st7789_rotation_t rotation;
        st7789_color_format_t color_format;
        bool invert_colors;
    };

    /**
     * @brief Initialize ST7789 display
     * @param config Hardware configuration
     * @return 0 on success
     */
    int st7789_init(const struct st7789_config *config);

    /**
     * @brief Set display rotation
     * @param rotation Rotation setting
     * @return 0 on success
     */
    int st7789_set_rotation(st7789_rotation_t rotation);

    /**
     * @brief Set display window
     * @param x0 Start X
     * @param y0 Start Y
     * @param x1 End X
     * @param y1 End Y
     * @return 0 on success
     */
    int st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

    /**
     * @brief Write pixel data to display
     * @param data RGB565 pixel data
     * @param len Number of pixels
     * @return 0 on success
     */
    int st7789_write_pixels(const uint16_t *data, size_t len);

    /**
     * @brief Fill rectangle with color
     * @param x Start X
     * @param y Start Y
     * @param w Width
     * @param h Height
     * @param color RGB565 color
     * @return 0 on success
     */
    int st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

    /**
     * @brief Clear display with color
     * @param color RGB565 color
     * @return 0 on success
     */
    int st7789_clear(uint16_t color);

    /**
     * @brief Draw single pixel
     * @param x X coordinate
     * @param y Y coordinate
     * @param color RGB565 color
     * @return 0 on success
     */
    int st7789_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

    /**
     * @brief Draw bitmap
     * @param x Start X
     * @param y Start Y
     * @param w Width
     * @param h Height
     * @param bitmap RGB565 bitmap data
     * @return 0 on success
     */
    int st7789_draw_bitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           const uint16_t *bitmap);

    /**
     * @brief Set backlight level
     * @param level 0-100
     * @return 0 on success
     */
    int st7789_set_backlight(uint8_t level);

    /**
     * @brief Enable/disable display
     * @param on true to enable
     * @return 0 on success
     */
    int st7789_display_on(bool on);

    /**
     * @brief Enter sleep mode
     * @return 0 on success
     */
    int st7789_sleep(void);

    /**
     * @brief Exit sleep mode
     * @return 0 on success
     */
    int st7789_wake(void);

    /**
     * @brief Invert display colors
     * @param invert true to invert
     * @return 0 on success
     */
    int st7789_invert(bool invert);

    /**
     * @brief Get display width
     * @return Width in pixels
     */
    uint16_t st7789_get_width(void);

    /**
     * @brief Get display height
     * @return Height in pixels
     */
    uint16_t st7789_get_height(void);

/* Color conversion helpers */
#define ST7789_RGB565(r, g, b) \
    ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

/* Common colors */
#define ST7789_BLACK 0x0000
#define ST7789_WHITE 0xFFFF
#define ST7789_RED 0xF800
#define ST7789_GREEN 0x07E0
#define ST7789_BLUE 0x001F
#define ST7789_YELLOW 0xFFE0
#define ST7789_CYAN 0x07FF
#define ST7789_MAGENTA 0xF81F
#define ST7789_ORANGE 0xFD20

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_ST7789_H */
