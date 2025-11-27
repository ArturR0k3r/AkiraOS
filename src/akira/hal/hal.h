/**
 * @file hal.h
 * @brief AkiraOS Hardware Abstraction Layer
 *
 * Provides unified hardware abstraction across different platforms
 * (ESP32, nRF, STM32, etc.) using Zephyr's device/driver model.
 */

#ifndef AKIRA_HAL_HAL_H
#define AKIRA_HAL_HAL_H

#include "../kernel/types.h"
#include <zephyr/device.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*===========================================================================*/
    /* HAL Types                                                                 */
    /*===========================================================================*/

    /** HAL device types */
    typedef enum
    {
        AKIRA_HAL_GPIO,
        AKIRA_HAL_SPI,
        AKIRA_HAL_I2C,
        AKIRA_HAL_UART,
        AKIRA_HAL_PWM,
        AKIRA_HAL_ADC,
        AKIRA_HAL_TIMER,
        AKIRA_HAL_FLASH,
        AKIRA_HAL_RTC,
        AKIRA_HAL_WDT,
        AKIRA_HAL_DISPLAY,
        AKIRA_HAL_SENSOR,
        AKIRA_HAL_RF,
        AKIRA_HAL_CRYPTO,
        AKIRA_HAL_USB,
        AKIRA_HAL_BT,
        AKIRA_HAL_WIFI
    } akira_hal_type_t;

    /** GPIO direction */
    typedef enum
    {
        AKIRA_GPIO_INPUT,
        AKIRA_GPIO_OUTPUT,
        AKIRA_GPIO_INPUT_PULLUP,
        AKIRA_GPIO_INPUT_PULLDOWN,
        AKIRA_GPIO_OUTPUT_OPEN_DRAIN
    } akira_hal_gpio_dir_t;

    /** GPIO interrupt trigger */
    typedef enum
    {
        AKIRA_GPIO_INT_DISABLE,
        AKIRA_GPIO_INT_RISING,
        AKIRA_GPIO_INT_FALLING,
        AKIRA_GPIO_INT_BOTH,
        AKIRA_GPIO_INT_LOW,
        AKIRA_GPIO_INT_HIGH
    } akira_hal_gpio_int_t;

    /** SPI mode */
    typedef enum
    {
        AKIRA_SPI_MODE_0, /* CPOL=0, CPHA=0 */
        AKIRA_SPI_MODE_1, /* CPOL=0, CPHA=1 */
        AKIRA_SPI_MODE_2, /* CPOL=1, CPHA=0 */
        AKIRA_SPI_MODE_3  /* CPOL=1, CPHA=1 */
    } akira_spi_mode_t;

    /** I2C speed */
    typedef enum
    {
        AKIRA_I2C_SPEED_STANDARD,  /* 100 kHz */
        AKIRA_I2C_SPEED_FAST,      /* 400 kHz */
        AKIRA_I2C_SPEED_FAST_PLUS, /* 1 MHz */
        AKIRA_I2C_SPEED_HIGH       /* 3.4 MHz */
    } akira_hal_i2c_speed_t;

    /*===========================================================================*/
    /* HAL Configuration                                                         */
    /*===========================================================================*/

    /** GPIO configuration */
    typedef struct
    {
        const char *port;     /**< GPIO port name */
        uint8_t pin;          /**< Pin number */
        akira_hal_gpio_dir_t dir; /**< Direction */
        bool initial_value;   /**< Initial output value */
    } akira_hal_gpio_config_t;

    /** SPI configuration */
    typedef struct
    {
        const char *bus;        /**< SPI bus name */
        uint32_t frequency;     /**< Clock frequency Hz */
        akira_spi_mode_t mode;  /**< SPI mode */
        uint8_t bits_per_word;  /**< Bits per word (usually 8) */
        akira_hal_gpio_config_t cs; /**< Chip select GPIO */
    } akira_spi_config_t;

    /** I2C configuration */
    typedef struct
    {
        const char *bus;         /**< I2C bus name */
        uint16_t address;        /**< Device address */
        akira_hal_i2c_speed_t speed; /**< Bus speed */
    } akira_hal_i2c_config_t;

    /*===========================================================================*/
    /* HAL Handles                                                               */
    /*===========================================================================*/

    /** GPIO handle */
    typedef struct akira_gpio akira_hal_gpio_t;

    /** SPI handle */
    typedef struct akira_spi akira_spi_t;

    /** I2C handle */
    typedef struct akira_i2c akira_hal_i2c_t;

    /*===========================================================================*/
    /* GPIO Callback                                                             */
    /*===========================================================================*/

    /**
     * @brief GPIO interrupt callback
     * @param gpio GPIO that triggered
     * @param pin Pin number
     * @param user_data User context
     */
    typedef void (*akira_hal_gpio_callback_t)(akira_hal_gpio_t *gpio, uint8_t pin,
                                          void *user_data);

    /*===========================================================================*/
    /* HAL Core API                                                              */
    /*===========================================================================*/

    /**
     * @brief Initialize the HAL layer
     * @return 0 on success, negative on error
     */
    int akira_core_hal_init(void);

    /**
     * @brief Get platform name
     * @return Platform name string
     */
    const char *akira_hal_platform(void);

    /**
     * @brief Get hardware revision
     * @return Hardware revision string
     */
    const char *akira_hal_hw_revision(void);

    /**
     * @brief Check if a HAL feature is available
     * @param type HAL device type
     * @return true if available
     */
    bool akira_hal_has_feature(akira_hal_type_t type);

    /**
     * @brief Get chip unique ID
     * @param id Output buffer (at least 16 bytes)
     * @param len Buffer length
     * @return Actual ID length
     */
    int akira_hal_chip_id(uint8_t *id, size_t len);

    /**
     * @brief Get chip temperature (if available)
     * @return Temperature in 0.1°C, or INT16_MIN if unavailable
     */
    int16_t akira_hal_chip_temp(void);

    /**
     * @brief Get supply voltage (if available)
     * @return Voltage in mV, or 0 if unavailable
     */
    uint16_t akira_hal_chip_voltage(void);

    /**
     * @brief Reset the chip
     */
    void akira_hal_reset(void);

    /*===========================================================================*/
    /* GPIO API                                                                  */
    /*===========================================================================*/

    /**
     * @brief Open a GPIO pin
     * @param config GPIO configuration
     * @return GPIO handle or NULL
     */
    akira_hal_gpio_t *akira_hal_gpio_open(const akira_hal_gpio_config_t *config);

    /**
     * @brief Close a GPIO pin
     * @param gpio GPIO handle
     */
    void akira_hal_gpio_close(akira_hal_gpio_t *gpio);

    /**
     * @brief Set GPIO output value
     * @param gpio GPIO handle
     * @param value Output value (0 or 1)
     * @return 0 on success
     */
    int akira_hal_gpio_set(akira_hal_gpio_t *gpio, bool value);

    /**
     * @brief Get GPIO input value
     * @param gpio GPIO handle
     * @return Input value (0 or 1), or negative on error
     */
    int akira_hal_gpio_get(akira_hal_gpio_t *gpio);

    /**
     * @brief Toggle GPIO output
     * @param gpio GPIO handle
     * @return 0 on success
     */
    int akira_hal_gpio_toggle(akira_hal_gpio_t *gpio);

    /**
     * @brief Configure GPIO interrupt
     * @param gpio GPIO handle
     * @param trigger Interrupt trigger type
     * @param callback Callback function
     * @param user_data User context
     * @return 0 on success
     */
    int akira_hal_gpio_set_interrupt(akira_hal_gpio_t *gpio, akira_hal_gpio_int_t trigger,
                                 akira_hal_gpio_callback_t callback, void *user_data);

    /*===========================================================================*/
    /* SPI API                                                                   */
    /*===========================================================================*/

    /**
     * @brief Open a SPI device
     * @param config SPI configuration
     * @return SPI handle or NULL
     */
    akira_spi_t *akira_hal_spi_open(const akira_spi_config_t *config);

    /**
     * @brief Close a SPI device
     * @param spi SPI handle
     */
    void akira_hal_spi_close(akira_spi_t *spi);

    /**
     * @brief SPI transfer (full duplex)
     * @param spi SPI handle
     * @param tx_data Data to transmit (can be NULL)
     * @param rx_data Receive buffer (can be NULL)
     * @param len Transfer length
     * @return 0 on success
     */
    int akira_hal_spi_transfer(akira_spi_t *spi, const uint8_t *tx_data,
                           uint8_t *rx_data, size_t len);

    /**
     * @brief SPI write
     * @param spi SPI handle
     * @param data Data to write
     * @param len Data length
     * @return 0 on success
     */
    int akira_hal_spi_write(akira_spi_t *spi, const uint8_t *data, size_t len);

    /**
     * @brief SPI read
     * @param spi SPI handle
     * @param data Read buffer
     * @param len Read length
     * @return 0 on success
     */
    int akira_hal_spi_read(akira_spi_t *spi, uint8_t *data, size_t len);

    /**
     * @brief SPI write then read
     * @param spi SPI handle
     * @param tx_data Data to write
     * @param tx_len Write length
     * @param rx_data Read buffer
     * @param rx_len Read length
     * @return 0 on success
     */
    int akira_hal_spi_write_read(akira_spi_t *spi,
                             const uint8_t *tx_data, size_t tx_len,
                             uint8_t *rx_data, size_t rx_len);

    /*===========================================================================*/
    /* I2C API                                                                   */
    /*===========================================================================*/

    /**
     * @brief Open an I2C device
     * @param config I2C configuration
     * @return I2C handle or NULL
     */
    akira_hal_i2c_t *akira_hal_i2c_open(const akira_hal_i2c_config_t *config);

    /**
     * @brief Close an I2C device
     * @param i2c I2C handle
     */
    void akira_hal_i2c_close(akira_hal_i2c_t *i2c);

    /**
     * @brief I2C write
     * @param i2c I2C handle
     * @param data Data to write
     * @param len Data length
     * @return 0 on success
     */
    int akira_hal_i2c_write(akira_hal_i2c_t *i2c, const uint8_t *data, size_t len);

    /**
     * @brief I2C read
     * @param i2c I2C handle
     * @param data Read buffer
     * @param len Read length
     * @return 0 on success
     */
    int akira_hal_i2c_read(akira_hal_i2c_t *i2c, uint8_t *data, size_t len);

    /**
     * @brief I2C write then read
     * @param i2c I2C handle
     * @param tx_data Data to write
     * @param tx_len Write length
     * @param rx_data Read buffer
     * @param rx_len Read length
     * @return 0 on success
     */
    int akira_hal_i2c_write_read(akira_hal_i2c_t *i2c,
                             const uint8_t *tx_data, size_t tx_len,
                             uint8_t *rx_data, size_t rx_len);

    /**
     * @brief I2C register read
     * @param i2c I2C handle
     * @param reg Register address
     * @param data Read buffer
     * @param len Read length
     * @return 0 on success
     */
    int akira_hal_i2c_read_reg(akira_hal_i2c_t *i2c, uint8_t reg,
                           uint8_t *data, size_t len);

    /**
     * @brief I2C register write
     * @param i2c I2C handle
     * @param reg Register address
     * @param data Data to write
     * @param len Data length
     * @return 0 on success
     */
    int akira_hal_i2c_write_reg(akira_hal_i2c_t *i2c, uint8_t reg,
                            const uint8_t *data, size_t len);

    /**
     * @brief Scan I2C bus for devices
     * @param bus I2C bus name
     * @param addresses Output buffer for found addresses
     * @param max_count Maximum addresses to return
     * @return Number of devices found
     */
    int akira_hal_i2c_scan(const char *bus, uint8_t *addresses, size_t max_count);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_HAL_HAL_H */
