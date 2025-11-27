/**
 * @file hal.c
 * @brief AkiraOS Hardware Abstraction Layer Implementation
 */

#include "hal.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_core_hal, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Internal Structures                                                       */
/*===========================================================================*/

struct akira_gpio
{
    const struct device *port;
    uint8_t pin;
    akira_hal_gpio_dir_t dir;
    struct gpio_callback cb;
    akira_hal_gpio_callback_t user_cb;
    void *user_data;
};

struct akira_spi
{
    const struct device *bus;
    struct spi_config config;
    struct gpio_dt_spec cs;
};

struct akira_i2c
{
    const struct device *bus;
    uint16_t address;
};

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    char platform[32];
} hal_state;

/*===========================================================================*/
/* HAL Core Implementation                                                   */
/*===========================================================================*/

int akira_core_hal_init(void)
{
    if (hal_state.initialized)
    {
        return 0;
    }

    LOG_INF("Initializing HAL layer");

#if defined(CONFIG_SOC_SERIES_ESP32)
    strncpy(hal_state.platform, "ESP32", sizeof(hal_state.platform));
#elif defined(CONFIG_SOC_SERIES_ESP32S3)
    strncpy(hal_state.platform, "ESP32-S3", sizeof(hal_state.platform));
#elif defined(CONFIG_SOC_SERIES_ESP32C3)
    strncpy(hal_state.platform, "ESP32-C3", sizeof(hal_state.platform));
#elif defined(CONFIG_SOC_SERIES_NRF52X)
    strncpy(hal_state.platform, "nRF52", sizeof(hal_state.platform));
#elif defined(CONFIG_SOC_SERIES_NRF53X)
    strncpy(hal_state.platform, "nRF53", sizeof(hal_state.platform));
#elif defined(CONFIG_SOC_SERIES_STM32F4X)
    strncpy(hal_state.platform, "STM32F4", sizeof(hal_state.platform));
#elif defined(CONFIG_BOARD_NATIVE_SIM)
    strncpy(hal_state.platform, "Native Sim", sizeof(hal_state.platform));
#else
    strncpy(hal_state.platform, "Unknown", sizeof(hal_state.platform));
#endif

    hal_state.initialized = true;

    LOG_INF("HAL initialized for platform: %s", hal_state.platform);

    return 0;
}

const char *akira_hal_platform(void)
{
    return hal_state.platform;
}

const char *akira_hal_hw_revision(void)
{
    return "1.0"; /* TODO: Read from device tree or flash */
}

bool akira_hal_has_feature(akira_hal_type_t type)
{
    switch (type)
    {
    case AKIRA_HAL_GPIO:
        return true; /* Always available */

    case AKIRA_HAL_SPI:
#if defined(CONFIG_SPI)
        return true;
#else
        return false;
#endif

    case AKIRA_HAL_I2C:
#if defined(CONFIG_I2C)
        return true;
#else
        return false;
#endif

    case AKIRA_HAL_WIFI:
#if defined(CONFIG_WIFI)
        return true;
#else
        return false;
#endif

    case AKIRA_HAL_BT:
#if defined(CONFIG_BT)
        return true;
#else
        return false;
#endif

    case AKIRA_HAL_DISPLAY:
#if defined(CONFIG_DISPLAY)
        return true;
#else
        return false;
#endif

    default:
        return false;
    }
}

int akira_hal_chip_id(uint8_t *id, size_t len)
{
    if (!id || len == 0)
    {
        return 0;
    }

    /* TODO: Read actual chip ID */
    memset(id, 0xAA, len);

    return len;
}

int16_t akira_hal_chip_temp(void)
{
    /* TODO: Read from internal temperature sensor */
    return INT16_MIN;
}

uint16_t akira_hal_chip_voltage(void)
{
    /* TODO: Read from internal voltage reference */
    return 0;
}

void akira_hal_reset(void)
{
    LOG_WRN("System reset requested");
    sys_reboot(SYS_REBOOT_COLD);
}

/*===========================================================================*/
/* GPIO Implementation                                                       */
/*===========================================================================*/

akira_hal_gpio_t *akira_hal_gpio_open(const akira_hal_gpio_config_t *config)
{
    if (!config || !config->port)
    {
        return NULL;
    }

    const struct device *port = device_get_binding(config->port);
    if (!port)
    {
        LOG_ERR("GPIO port '%s' not found", config->port);
        return NULL;
    }

    akira_hal_gpio_t *gpio = k_malloc(sizeof(akira_hal_gpio_t));
    if (!gpio)
    {
        return NULL;
    }

    memset(gpio, 0, sizeof(*gpio));
    gpio->port = port;
    gpio->pin = config->pin;
    gpio->dir = config->dir;

    gpio_flags_t flags = 0;

    switch (config->dir)
    {
    case AKIRA_GPIO_INPUT:
        flags = GPIO_INPUT;
        break;
    case AKIRA_GPIO_INPUT_PULLUP:
        flags = GPIO_INPUT | GPIO_PULL_UP;
        break;
    case AKIRA_GPIO_INPUT_PULLDOWN:
        flags = GPIO_INPUT | GPIO_PULL_DOWN;
        break;
    case AKIRA_GPIO_OUTPUT:
        flags = GPIO_OUTPUT | (config->initial_value ? GPIO_OUTPUT_HIGH : GPIO_OUTPUT_LOW);
        break;
    case AKIRA_GPIO_OUTPUT_OPEN_DRAIN:
        flags = GPIO_OUTPUT | GPIO_OPEN_DRAIN;
        break;
    }

    int ret = gpio_pin_configure(port, config->pin, flags);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure GPIO %s:%d", config->port, config->pin);
        k_free(gpio);
        return NULL;
    }

    LOG_DBG("Opened GPIO %s:%d", config->port, config->pin);

    return gpio;
}

void akira_hal_gpio_close(akira_hal_gpio_t *gpio)
{
    if (!gpio)
        return;

    gpio_pin_configure(gpio->port, gpio->pin, GPIO_DISCONNECTED);
    k_free(gpio);
}

int akira_hal_gpio_set(akira_hal_gpio_t *gpio, bool value)
{
    if (!gpio)
        return -1;
    return gpio_pin_set(gpio->port, gpio->pin, value ? 1 : 0);
}

int akira_hal_gpio_get(akira_hal_gpio_t *gpio)
{
    if (!gpio)
        return -1;
    return gpio_pin_get(gpio->port, gpio->pin);
}

int akira_hal_gpio_toggle(akira_hal_gpio_t *gpio)
{
    if (!gpio)
        return -1;
    return gpio_pin_toggle(gpio->port, gpio->pin);
}

static void gpio_interrupt_handler(const struct device *port,
                                   struct gpio_callback *cb,
                                   gpio_port_pins_t pins)
{
    akira_hal_gpio_t *gpio = CONTAINER_OF(cb, akira_hal_gpio_t, cb);
    if (gpio->user_cb)
    {
        gpio->user_cb(gpio, gpio->pin, gpio->user_data);
    }
}

int akira_hal_gpio_set_interrupt(akira_hal_gpio_t *gpio, akira_hal_gpio_int_t trigger,
                                 akira_hal_gpio_callback_t callback, void *user_data)
{
    if (!gpio)
        return -1;

    gpio->user_cb = callback;
    gpio->user_data = user_data;

    gpio_flags_t flags;

    switch (trigger)
    {
    case AKIRA_GPIO_INT_DISABLE:
        gpio_pin_interrupt_configure(gpio->port, gpio->pin, GPIO_INT_DISABLE);
        return 0;
    case AKIRA_GPIO_INT_RISING:
        flags = GPIO_INT_EDGE_RISING;
        break;
    case AKIRA_GPIO_INT_FALLING:
        flags = GPIO_INT_EDGE_FALLING;
        break;
    case AKIRA_GPIO_INT_BOTH:
        flags = GPIO_INT_EDGE_BOTH;
        break;
    case AKIRA_GPIO_INT_LOW:
        flags = GPIO_INT_LEVEL_LOW;
        break;
    case AKIRA_GPIO_INT_HIGH:
        flags = GPIO_INT_LEVEL_HIGH;
        break;
    default:
        return -1;
    }

    gpio_init_callback(&gpio->cb, gpio_interrupt_handler, BIT(gpio->pin));
    gpio_add_callback(gpio->port, &gpio->cb);

    return gpio_pin_interrupt_configure(gpio->port, gpio->pin, flags);
}

/*===========================================================================*/
/* SPI Implementation                                                        */
/*===========================================================================*/

akira_spi_t *akira_hal_spi_open(const akira_spi_config_t *config)
{
    if (!config || !config->bus)
    {
        return NULL;
    }

    const struct device *bus = device_get_binding(config->bus);
    if (!bus)
    {
        LOG_ERR("SPI bus '%s' not found", config->bus);
        return NULL;
    }

    akira_spi_t *spi = k_malloc(sizeof(akira_spi_t));
    if (!spi)
    {
        return NULL;
    }

    memset(spi, 0, sizeof(*spi));
    spi->bus = bus;

    spi->config.frequency = config->frequency;
    spi->config.operation = SPI_WORD_SET(config->bits_per_word);

    switch (config->mode)
    {
    case AKIRA_SPI_MODE_0:
        /* Default */
        break;
    case AKIRA_SPI_MODE_1:
        spi->config.operation |= SPI_MODE_CPHA;
        break;
    case AKIRA_SPI_MODE_2:
        spi->config.operation |= SPI_MODE_CPOL;
        break;
    case AKIRA_SPI_MODE_3:
        spi->config.operation |= SPI_MODE_CPOL | SPI_MODE_CPHA;
        break;
    }

    /* TODO: Configure CS GPIO */

    LOG_DBG("Opened SPI %s @ %u Hz", config->bus, config->frequency);

    return spi;
}

void akira_hal_spi_close(akira_spi_t *spi)
{
    if (spi)
    {
        k_free(spi);
    }
}

int akira_hal_spi_transfer(akira_spi_t *spi, const uint8_t *tx_data,
                           uint8_t *rx_data, size_t len)
{
    if (!spi)
        return -1;

    struct spi_buf tx_buf = {.buf = (void *)tx_data, .len = len};
    struct spi_buf_set tx = {.buffers = &tx_buf, .count = tx_data ? 1 : 0};

    struct spi_buf rx_buf = {.buf = rx_data, .len = len};
    struct spi_buf_set rx = {.buffers = &rx_buf, .count = rx_data ? 1 : 0};

    return spi_transceive(spi->bus, &spi->config, &tx, &rx);
}

int akira_hal_spi_write(akira_spi_t *spi, const uint8_t *data, size_t len)
{
    return akira_hal_spi_transfer(spi, data, NULL, len);
}

int akira_hal_spi_read(akira_spi_t *spi, uint8_t *data, size_t len)
{
    return akira_hal_spi_transfer(spi, NULL, data, len);
}

int akira_hal_spi_write_read(akira_spi_t *spi,
                             const uint8_t *tx_data, size_t tx_len,
                             uint8_t *rx_data, size_t rx_len)
{
    if (!spi)
        return -1;

    struct spi_buf tx_buf = {.buf = (void *)tx_data, .len = tx_len};
    struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};

    int ret = spi_write(spi->bus, &spi->config, &tx);
    if (ret < 0)
        return ret;

    struct spi_buf rx_buf = {.buf = rx_data, .len = rx_len};
    struct spi_buf_set rx = {.buffers = &rx_buf, .count = 1};

    return spi_read(spi->bus, &spi->config, &rx);
}

/*===========================================================================*/
/* I2C Implementation                                                        */
/*===========================================================================*/

akira_hal_i2c_t *akira_hal_i2c_open(const akira_hal_i2c_config_t *config)
{
    if (!config || !config->bus)
    {
        return NULL;
    }

    const struct device *bus = device_get_binding(config->bus);
    if (!bus)
    {
        LOG_ERR("I2C bus '%s' not found", config->bus);
        return NULL;
    }

    akira_hal_i2c_t *i2c = k_malloc(sizeof(akira_hal_i2c_t));
    if (!i2c)
    {
        return NULL;
    }

    i2c->bus = bus;
    i2c->address = config->address;

    /* Configure speed */
    uint32_t speed;
    switch (config->speed)
    {
    case AKIRA_I2C_SPEED_STANDARD:
        speed = I2C_SPEED_STANDARD;
        break;
    case AKIRA_I2C_SPEED_FAST:
        speed = I2C_SPEED_FAST;
        break;
    case AKIRA_I2C_SPEED_FAST_PLUS:
        speed = I2C_SPEED_FAST_PLUS;
        break;
    case AKIRA_I2C_SPEED_HIGH:
        speed = I2C_SPEED_HIGH;
        break;
    default:
        speed = I2C_SPEED_STANDARD;
    }

    i2c_configure(bus, I2C_MODE_CONTROLLER | I2C_SPEED_SET(speed));

    LOG_DBG("Opened I2C %s @ 0x%02X", config->bus, config->address);

    return i2c;
}

void akira_hal_i2c_close(akira_hal_i2c_t *i2c)
{
    if (i2c)
    {
        k_free(i2c);
    }
}

int akira_hal_i2c_write(akira_hal_i2c_t *i2c, const uint8_t *data, size_t len)
{
    if (!i2c)
        return -1;
    return i2c_write(i2c->bus, data, len, i2c->address);
}

int akira_hal_i2c_read(akira_hal_i2c_t *i2c, uint8_t *data, size_t len)
{
    if (!i2c)
        return -1;
    return i2c_read(i2c->bus, data, len, i2c->address);
}

int akira_hal_i2c_write_read(akira_hal_i2c_t *i2c,
                             const uint8_t *tx_data, size_t tx_len,
                             uint8_t *rx_data, size_t rx_len)
{
    if (!i2c)
        return -1;
    return i2c_write_read(i2c->bus, i2c->address,
                          tx_data, tx_len, rx_data, rx_len);
}

int akira_hal_i2c_read_reg(akira_hal_i2c_t *i2c, uint8_t reg,
                           uint8_t *data, size_t len)
{
    return akira_hal_i2c_write_read(i2c, &reg, 1, data, len);
}

int akira_hal_i2c_write_reg(akira_hal_i2c_t *i2c, uint8_t reg,
                            const uint8_t *data, size_t len)
{
    if (!i2c)
        return -1;

    uint8_t buf[len + 1];
    buf[0] = reg;
    memcpy(&buf[1], data, len);

    return akira_hal_i2c_write(i2c, buf, len + 1);
}

int akira_hal_i2c_scan(const char *bus, uint8_t *addresses, size_t max_count)
{
    const struct device *dev = device_get_binding(bus);
    if (!dev)
    {
        return 0;
    }

    int count = 0;
    uint8_t dummy;

    for (uint8_t addr = 0x08; addr < 0x78 && count < max_count; addr++)
    {
        if (i2c_read(dev, &dummy, 0, addr) == 0)
        {
            addresses[count++] = addr;
            LOG_INF("I2C device found at 0x%02X", addr);
        }
    }

    return count;
}
