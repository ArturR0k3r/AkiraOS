#pragma once
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

#define ILI9341_DISPLAY_WIDTH 240
#define ILI9341_DISPLAY_HEIGHT 320
// Pin mapping for Akira Console
#define ILI9341_MISO_PIN 25
#define ILI9341_MOSI_PIN 23
#define ILI9341_SCK_PIN 18
#define ILI9341_CS_PIN 22
#define ILI9341_DC_PIN 21
#define ILI9341_RESET_PIN 18
#define ILI9341_BL_PIN 27

// Display colors (RGB565 format)
#define WHITE_COLOR 0xFFFF
#define RED_COLOR 0xF800
#define GREEN_COLOR 0x07E0
#define BLUE_COLOR 0x001F
#define BLACK_COLOR 0x0000
#define YELLOW_COLOR 0xFFE0
#define MAGENTA_COLOR 0xF81F
#define CYAN_COLOR 0x07FF

// GPIO pin definitions
#define DC_GPIO_PIN ILI9341_DC_PIN
#define RESET_GPIO_PIN ILI9341_RESET_PIN

// ILI9341 Commands
#define ILI9341_SWRESET 0x01
#define ILI9341_SLPOUT 0x11
#define ILI9341_DISPOFF 0x28
#define ILI9341_DISPON 0x29
#define ILI9341_CASET 0x2A
#define ILI9341_PASET 0x2B
#define ILI9341_RAMWR 0x2C
#define ILI9341_MADCTL 0x36
#define ILI9341_COLMOD 0x3A
#define ILI9341_PWCTR1 0xC0
#define ILI9341_PWCTR2 0xC1
#define ILI9341_VMCTR1 0xC5
#define ILI9341_VMCTR2 0xC7
#define ILI9341_GMCTRP1 0xE0
#define ILI9341_GMCTRN1 0xE1

int ili9341_init(const struct device *spi_dev, const struct device *gpio_dev,
                 struct spi_config *spi_cfg);
int ili9341_fill_color(uint16_t color);
int ili9341_draw_color_bars(void);
int ili9341_draw_test_pattern(void);
int ili9341_backlight_init(const struct device *gpio_dev, int pin);
void ili9341_draw_text(int x, int y, const char *text, uint16_t color);
void ili9341_draw_pixel(int x, int y, uint16_t color);
void ili9341_crt_screensaver(void);