/**
 * @file ssd1306.h
 * @brief SSD1306 OLED Display Driver
 */

#ifndef AKIRA_SSD1306_H
#define AKIRA_SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SSD1306 configuration
 */
struct ssd1306_config {
	const struct device *i2c_dev;  // or spi_dev
	uint16_t i2c_addr;             // 0x3C or 0x3D
	uint8_t width;                 // 128
	uint8_t height;                // 32 or 64
	bool external_vcc;
};

/**
 * @brief Initialize SSD1306 display
 * @param config Hardware configuration
 * @return 0 on success
 */
int ssd1306_init(const struct ssd1306_config *config);

/**
 * @brief Clear display
 */
void ssd1306_clear(void);

/**
 * @brief Draw pixel
 * @param x X coordinate
 * @param y Y coordinate
 * @param color 0=off, 1=on
 */
void ssd1306_pixel(int x, int y, uint8_t color);

/**
 * @brief Draw text
 * @param x X coordinate
 * @param y Y coordinate
 * @param text Text string
 */
void ssd1306_text(int x, int y, const char *text);

/**
 * @brief Update display from framebuffer
 */
void ssd1306_update(void);

/**
 * @brief Set display contrast
 * @param contrast 0-255
 */
void ssd1306_set_contrast(uint8_t contrast);

/**
 * @brief Invert display
 * @param invert true to invert
 */
void ssd1306_invert(bool invert);

/**
 * @brief Turn display on/off
 * @param on true to turn on
 */
void ssd1306_power(bool on);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SSD1306_H */
