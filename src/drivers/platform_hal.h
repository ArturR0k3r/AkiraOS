/**
 * @file platform_hal.h
 * @brief Platform Hardware Abstraction Layer
 *
 * Provides hardware-specific abstraction for GPIO, SPI, display simulation.
 * - native_sim (simulation/testing with button/display emulation)
 * - ESP32 (original ESP32)
 * - ESP32-S3 (newer ESP32-S3)
 * - STM32 (ST microcontrollers)
 * - Nordic nRF (nRF52/nRF54 series)
 *
 * This allows the same codebase to compile and run on all platforms.
 */

#ifndef PLATFORM_HAL_H
#define PLATFORM_HAL_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <stdbool.h>
#include <stdint.h>

/* Platform detection */
#if defined(CONFIG_BOARD_NATIVE_SIM)
#define AKIRA_PLATFORM_NATIVE_SIM 1
#define AKIRA_PLATFORM_ESP32      0
#define AKIRA_PLATFORM_ESP32S3    0
#define AKIRA_PLATFORM_ESP32C6    0
#define AKIRA_PLATFORM_ESP32H2    0
#define AKIRA_PLATFORM_STM32      0
#define AKIRA_PLATFORM_NORDIC     0
#elif defined(CONFIG_SOC_ESP32S3)
#define AKIRA_PLATFORM_NATIVE_SIM 0
#define AKIRA_PLATFORM_ESP32      0
#define AKIRA_PLATFORM_ESP32S3    1
#define AKIRA_PLATFORM_ESP32C6    0
#define AKIRA_PLATFORM_ESP32H2    0
#define AKIRA_PLATFORM_STM32      0
#define AKIRA_PLATFORM_NORDIC     0
#elif defined(CONFIG_SOC_ESP32C6)
#define AKIRA_PLATFORM_NATIVE_SIM 0
#define AKIRA_PLATFORM_ESP32      0
#define AKIRA_PLATFORM_ESP32S3    0
#define AKIRA_PLATFORM_ESP32C6    1
#define AKIRA_PLATFORM_ESP32H2    0
#define AKIRA_PLATFORM_STM32      0
#define AKIRA_PLATFORM_NORDIC     0
#elif defined(CONFIG_SOC_ESP32H2)
#define AKIRA_PLATFORM_NATIVE_SIM 0
#define AKIRA_PLATFORM_ESP32      0
#define AKIRA_PLATFORM_ESP32S3    0
#define AKIRA_PLATFORM_ESP32C6    0
#define AKIRA_PLATFORM_ESP32H2    1
#define AKIRA_PLATFORM_STM32      0
#define AKIRA_PLATFORM_NORDIC     0
#elif defined(CONFIG_SOC_ESP32)
#define AKIRA_PLATFORM_NATIVE_SIM 0
#define AKIRA_PLATFORM_ESP32      1
#define AKIRA_PLATFORM_ESP32S3    0
#define AKIRA_PLATFORM_ESP32C6    0
#define AKIRA_PLATFORM_ESP32H2    0
#define AKIRA_PLATFORM_STM32      0
#define AKIRA_PLATFORM_NORDIC     0
#elif defined(CONFIG_SOC_FAMILY_STM32)
#define AKIRA_PLATFORM_NATIVE_SIM 0
#define AKIRA_PLATFORM_ESP32      0
#define AKIRA_PLATFORM_ESP32S3    0
#define AKIRA_PLATFORM_ESP32C6    0
#define AKIRA_PLATFORM_ESP32H2    0
#define AKIRA_PLATFORM_STM32      1
#define AKIRA_PLATFORM_NORDIC     0
#elif defined(CONFIG_SOC_SERIES_NRF54LX) || defined(CONFIG_SOC_SERIES_NRF52X) || defined(CONFIG_SOC_SERIES_NRF53X)
#define AKIRA_PLATFORM_NATIVE_SIM 0
#define AKIRA_PLATFORM_ESP32      0
#define AKIRA_PLATFORM_ESP32S3    0
#define AKIRA_PLATFORM_ESP32C6    0
#define AKIRA_PLATFORM_ESP32H2    0
#define AKIRA_PLATFORM_STM32      0
#define AKIRA_PLATFORM_NORDIC     1
#else
#define AKIRA_PLATFORM_NATIVE_SIM 0
#define AKIRA_PLATFORM_ESP32      0
#define AKIRA_PLATFORM_ESP32S3    0
#define AKIRA_PLATFORM_ESP32C6    0
#define AKIRA_PLATFORM_ESP32H2    0
#define AKIRA_PLATFORM_STM32      0
#define AKIRA_PLATFORM_NORDIC     0
#warning "Unknown platform - assuming generic"
#endif

/* Use Kconfig symbols for feature detection, not compile-time SOC macros.
 * Boards enable features in their .conf file:
 *   CONFIG_AKIRA_DISPLAY=y  — display HAL + WASM exports active
 *   CONFIG_AKIRA_WIFI=y     — WiFi connectivity APIs active
 * Code should guard on CONFIG_DISPLAY / CONFIG_AKIRA_DISPLAY, etc.
 * The AKIRA_PLATFORM_* macros below remain available for the few places
 * that genuinely need platform-specific code paths (e.g. sim vs HW flush).
 *
 * Runtime feature queries (used where compile-time guards are inconvenient):
 */
bool akira_has_display(void);
bool akira_has_wifi(void);
bool akira_has_spi(void);
bool akira_has_gpio(void);

/* Platform-specific device names */
#if AKIRA_PLATFORM_NATIVE_SIM
#define AKIRA_GPIO_DEVICE_NAME "gpio0" // Simulated device
#define AKIRA_SPI_DEVICE_NAME "spi0"   // Simulated device
#else
#define AKIRA_GPIO_DEVICE_NAME "gpio0"
#define AKIRA_SPI_DEVICE_NAME "spi2"
#endif

/**
 * @brief Initialize the Akira HAL
 * @return 0 on success, negative errno on error
 */
int akira_hal_init(void);

/**
 * @brief Get the platform name string
 * @return Platform name (e.g., "native_sim", "ESP32", "ESP32-S3")
 */
const char *akira_get_platform_name(void);

/**
 * @brief Get GPIO device safely
 * @param label Device tree label
 * @return Device pointer or NULL
 */
const struct device *akira_get_gpio_device(const char *label);

/**
 * @brief Get SPI device safely
 * @param label Device tree label
 * @return Device pointer or NULL
 */
const struct device *akira_get_spi_device(const char *label);

/**
 * @brief Safe GPIO pin configure
 * @param dev GPIO device
 * @param pin Pin number
 * @param flags Configuration flags
 * @return 0 on success, negative errno on error
 */
int akira_gpio_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags);

/**
 * @brief Safe GPIO pin set
 * @param dev GPIO device
 * @param pin Pin number
 * @param value Pin value
 * @return 0 on success, negative errno on error
 */
int akira_gpio_pin_set(const struct device *dev, gpio_pin_t pin, int value);

/**
 * @brief Safe GPIO pin get (simulated on native_sim)
 * @param dev GPIO device
 * @param pin Pin number
 * @return Pin value
 */
int akira_gpio_pin_get(const struct device *dev, gpio_pin_t pin);

/**
 * @brief Safe SPI write (simulated on native_sim for display)
 * @param dev SPI device
 * @param config SPI configuration
 * @param tx_bufs Transmit buffers
 * @return 0 on success, negative errno on error
 */
int akira_spi_write(const struct device *dev, const struct spi_config *config,
                    const struct spi_buf_set *tx_bufs);

/**
 * @brief Read simulated button state (native_sim only)
 * @return Button bitmask
 */
uint32_t akira_sim_read_buttons(void);

/**
 * @brief Update simulated display buffer (native_sim only)
 * @param x X coordinate
 * @param y Y coordinate
 * @param color Color value (RGB565)
 */
void akira_sim_draw_pixel(int x, int y, uint16_t color);

/**
 * @brief Show simulated display window (native_sim only)
 */
void akira_sim_show_display(void);

/**
 * @brief Reset the system (cold reboot)
 */
void akira_hal_reset(void);

/**
 * @brief Get the platform name string (alias for akira_get_platform_name)
 * @return Platform name (e.g., "native_sim", "ESP32", "ESP32-S3")
 */
const char *akira_hal_platform(void);

/**
 * @brief Get pointer to hardware framebuffer (240x320 RGB565)
 * @return Pointer to framebuffer or NULL if not available
 */
uint16_t *akira_framebuffer_get(void);

/**
 * @brief Initialize hardware display HAL
 * @return 0 on success, negative errno on error
 */
int akira_display_hal_init(void);

/**
 * @brief Flush framebuffer to physical display hardware
 */
void akira_display_hal_flush(void);

/**
 * @brief Get display capabilities
 * @param caps Pointer to capabilities structure to fill
 * @return 0 on success, negative errno on error
 */
struct display_capabilities;
int akira_display_hal_get_capabilities(struct display_capabilities *caps);

/**
 * @brief Set display backlight brightness
 * @param brightness Brightness level (0-100)
 */
void akira_display_hal_set_brightness(uint8_t brightness);

/**
 * @brief Set display orientation/rotation
 * @param rotation Rotation mode (0=0°, 1=90°, 2=180°, 3=270°)
 * @return 0 on success, negative errno on error
 */
int akira_display_hal_set_rotation(uint8_t rotation);

/**
 * @brief Enable or disable display blanking (screen sleep/wake).
 * @param blank true = screen off, false = screen on.
 */
void akira_display_hal_set_blank(bool blank);

/**
 * @brief Write a packed RGB565 buffer directly to the display hardware,
 *        bypassing the OS framebuffer.
 * @param x,y   Top-left corner on the display
 * @param w,h   Width and height in pixels
 * @param data  Packed RGB565 data (pitch == w, no stride)
 * @return 0 on success, negative errno on error
 */
int akira_display_hal_write_raw(int x, int y, int w, int h, const uint16_t *data);

#endif /* PLATFORM_HAL_H */
