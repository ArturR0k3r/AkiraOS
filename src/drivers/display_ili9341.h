#ifndef __DISPLAY_ILI9341_H__
#define __DISPLAY_ILI9341_H__
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include "fonts.h"

#define ILI9341_DISPLAY_WIDTH 320
#define ILI9341_DISPLAY_HEIGHT 240

// ===== ESP32 to ILI9341 Pin Mapping =====
// SPI Pins (handled by pinctrl)
#define ILI9341_MOSI_PIN 23 // GPIO23 → ILI9341 MOSI (SDI)
#define ILI9341_MISO_PIN 25 // GPIO25 → ILI9341 MISO (SDO) - optional
#define ILI9341_SCK_PIN 19  // GPIO19 → ILI9341 SCK (FIXED from 18)

// Manual Control Pins
#define ILI9341_CS_PIN 22    // GPIO22 → ILI9341 CS
#define ILI9341_DC_PIN 21    // GPIO21 → ILI9341 DC (Data/Command)
#define ILI9341_RESET_PIN 18 // GPIO18 → ILI9341 RESET
#define ILI9341_BL_PIN 27    // GPIO27 → ILI9341 LED (Backlight)

// GPIO pin definitions for display (backwards compatibility)
#define DC_GPIO_PIN ILI9341_DC_PIN
#define RESET_GPIO_PIN ILI9341_RESET_PIN

// Display colors (RGB565 format)
#define WHITE_COLOR 0xFFFF
#define RED_COLOR 0xF800
#define GREEN_COLOR 0x07E0
#define BLUE_COLOR 0x001F
#define BLACK_COLOR 0x0000
#define YELLOW_COLOR 0xFFE0
#define MAGENTA_COLOR 0xF81F
#define CYAN_COLOR 0x07FF

// Additional colors for gaming UI
#define ORANGE_COLOR 0xFC00
#define PURPLE_COLOR 0x8010
#define PINK_COLOR 0xF81F
#define LIME_COLOR 0x87E0
#define NAVY_COLOR 0x000F
#define MAROON_COLOR 0x8000
#define OLIVE_COLOR 0x8400
#define GRAY_COLOR 0x8410
#define SILVER_COLOR 0xC618
#define DARKGREEN_COLOR 0x0320

// ILI9341 Command Set
#define ILI9341_SWRESET 0x01 // Software reset
#define ILI9341_SLPIN 0x10   // Sleep in
#define ILI9341_SLPOUT 0x11  // Sleep out
#define ILI9341_PTLON 0x12   // Partial mode on
#define ILI9341_NORON 0x13   // Normal display mode on
#define ILI9341_INVOFF 0x20  // Display inversion off
#define ILI9341_INVON 0x21   // Display inversion on
#define ILI9341_DISPOFF 0x28 // Display off
#define ILI9341_DISPON 0x29  // Display on
#define ILI9341_CASET 0x2A   // Column address set
#define ILI9341_PASET 0x2B   // Page address set
#define ILI9341_RAMWR 0x2C   // Memory write
#define ILI9341_RAMRD 0x2E   // Memory read
#define ILI9341_PTLAR 0x30   // Partial area
#define ILI9341_MADCTL 0x36  // Memory access control
#define ILI9341_COLMOD 0x3A  // Pixel format set
#define ILI9341_FRMCTR1 0xB1 // Frame rate control (normal mode)
#define ILI9341_FRMCTR2 0xB2 // Frame rate control (idle mode)
#define ILI9341_FRMCTR3 0xB3 // Frame rate control (partial mode)
#define ILI9341_INVCTR 0xB4  // Display inversion control
#define ILI9341_DFUNCTR 0xB6 // Display function control
#define ILI9341_PWCTR1 0xC0  // Power control 1
#define ILI9341_PWCTR2 0xC1  // Power control 2
#define ILI9341_PWCTR3 0xC2  // Power control 3
#define ILI9341_PWCTR4 0xC3  // Power control 4
#define ILI9341_PWCTR5 0xC4  // Power control 5
#define ILI9341_VMCTR1 0xC5  // VCOM control 1
#define ILI9341_VMCTR2 0xC7  // VCOM control 2
#define ILI9341_RDID1 0xDA   // Read ID 1
#define ILI9341_RDID2 0xDB   // Read ID 2
#define ILI9341_RDID3 0xDC   // Read ID 3
#define ILI9341_RDID4 0xDD   // Read ID 4
#define ILI9341_GMCTRP1 0xE0 // Positive gamma correction
#define ILI9341_GMCTRN1 0xE1 // Negative gamma correction

// Memory Access Control bits
#define ILI9341_MADCTL_MY 0x80  // Row address order
#define ILI9341_MADCTL_MX 0x40  // Column address order
#define ILI9341_MADCTL_MV 0x20  // Row/Column exchange
#define ILI9341_MADCTL_ML 0x10  // Vertical refresh order
#define ILI9341_MADCTL_BGR 0x08 // RGB-BGR order
#define ILI9341_MADCTL_MH 0x04  // Horizontal refresh order

// Function declarations
int ili9341_init(const struct device *spi_dev, const struct device *gpio_dev,
                 struct spi_config *spi_cfg);
int ili9341_fill_color(uint16_t color);
int ili9341_draw_color_bars(void);
int ili9341_draw_test_pattern(void);
int ili9341_backlight_init(const struct device *gpio_dev, int pin);
void ili9341_draw_text(int x, int y, const char *text, uint16_t color, FontType font);
void ili9341_draw_pixel(int x, int y, uint16_t color);
void ili9341_crt_screensaver(void);

// TO DO add fill_screen, clear_screen,

#endif // __DISPLAY_ILI9341_H__