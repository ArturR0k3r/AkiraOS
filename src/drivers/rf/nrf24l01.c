/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * NRF24L01+ Wireless Transceiver Driver Implementation
 */

#include "nrf24l01.h"
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(nrf24l01, CONFIG_LOG_DEFAULT_LEVEL);

static int nrf24_write_register(struct nrf24_config *config, uint8_t reg, uint8_t value)
{
    uint8_t tx_buf[2] = {NRF24_CMD_W_REGISTER | reg, value};
    const struct spi_buf tx = {.buf = tx_buf, .len = 2};
    const struct spi_buf_set tx_bufs = {.buffers = &tx, .count = 1};

    return spi_write(config->spi_dev, &config->spi_cfg, &tx_bufs);
}

static int nrf24_read_register(struct nrf24_config *config, uint8_t reg, uint8_t *value)
{
    uint8_t tx_buf[2] = {NRF24_CMD_R_REGISTER | reg, 0xFF};
    uint8_t rx_buf[2];
    const struct spi_buf tx = {.buf = tx_buf, .len = 2};
    const struct spi_buf_set tx_bufs = {.buffers = &tx, .count = 1};
    const struct spi_buf rx = {.buf = rx_buf, .len = 2};
    const struct spi_buf_set rx_bufs = {.buffers = &rx, .count = 1};

    int ret = spi_transceive(config->spi_dev, &config->spi_cfg, &tx_bufs, &rx_bufs);
    if (ret == 0)
    {
        *value = rx_buf[1];
    }
    return ret;
}

static int nrf24_write_register_multi(struct nrf24_config *config, uint8_t reg,
                                      const uint8_t *data, size_t len)
{
    uint8_t tx_buf[33];
    tx_buf[0] = NRF24_CMD_W_REGISTER | reg;
    memcpy(&tx_buf[1], data, len);

    const struct spi_buf tx = {.buf = tx_buf, .len = len + 1};
    const struct spi_buf_set tx_bufs = {.buffers = &tx, .count = 1};

    return spi_write(config->spi_dev, &config->spi_cfg, &tx_bufs);
}

static int nrf24_send_command(struct nrf24_config *config, uint8_t cmd)
{
    const struct spi_buf tx = {.buf = &cmd, .len = 1};
    const struct spi_buf_set tx_bufs = {.buffers = &tx, .count = 1};

    return spi_write(config->spi_dev, &config->spi_cfg, &tx_bufs);
}

int nrf24_init(struct nrf24_config *config)
{
    int ret;

    if (!config || !config->spi_dev || !config->gpio_dev)
    {
        return -EINVAL;
    }

    /* Configure CE pin as output */
    ret = gpio_pin_configure(config->gpio_dev, config->ce_pin, GPIO_OUTPUT_LOW);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure CE pin: %d", ret);
        return ret;
    }

    /* Configure IRQ pin as input (optional) */
    if (config->irq_pin != 0xFF)
    {
        ret = gpio_pin_configure(config->gpio_dev, config->irq_pin, GPIO_INPUT);
        if (ret < 0)
        {
            LOG_WRN("Failed to configure IRQ pin: %d", ret);
        }
    }

    /* Power down first */
    k_msleep(10);
    nrf24_write_register(config, NRF24_REG_CONFIG, 0x00);
    k_msleep(10);

    /* Set channel */
    nrf24_write_register(config, NRF24_REG_RF_CH, config->channel);

    /* Set data rate and power */
    uint8_t rf_setup = 0;
    switch (config->data_rate)
    {
    case NRF24_DATA_RATE_250KBPS:
        rf_setup |= 0x20;
        break;
    case NRF24_DATA_RATE_2MBPS:
        rf_setup |= 0x08;
        break;
    default:
        break;
    }

    switch (config->power)
    {
    case NRF24_POWER_0DBM:
        rf_setup |= 0x06;
        break;
    case NRF24_POWER_MINUS_6DBM:
        rf_setup |= 0x04;
        break;
    case NRF24_POWER_MINUS_12DBM:
        rf_setup |= 0x02;
        break;
    default:
        break;
    }

    nrf24_write_register(config, NRF24_REG_RF_SETUP, rf_setup);

    /* Set address width */
    nrf24_write_register(config, NRF24_REG_SETUP_AW, config->address_width - 2);

    /* Enable auto acknowledgment on pipe 0 */
    nrf24_write_register(config, NRF24_REG_EN_AA, 0x01);

    /* Enable RX pipe 0 */
    nrf24_write_register(config, NRF24_REG_EN_RXADDR, 0x01);

    /* Set retransmit delay and count */
    nrf24_write_register(config, NRF24_REG_SETUP_RETR, 0x1F);

    /* Set payload size for pipe 0 */
    nrf24_write_register(config, NRF24_REG_RX_PW_P0, 32);

    /* Clear interrupts */
    nrf24_write_register(config, NRF24_REG_STATUS, 0x70);

    /* Flush FIFOs */
    nrf24_send_command(config, NRF24_CMD_FLUSH_TX);
    nrf24_send_command(config, NRF24_CMD_FLUSH_RX);

    LOG_INF("NRF24L01+ initialized (channel=%d, rate=%d, power=%d)",
            config->channel, config->data_rate, config->power);

    return 0;
}

int nrf24_rx_mode(struct nrf24_config *config)
{
    gpio_pin_set(config->gpio_dev, config->ce_pin, 0);

    uint8_t cfg = NRF24_CONFIG_EN_CRC | NRF24_CONFIG_CRCO |
                  NRF24_CONFIG_PWR_UP | NRF24_CONFIG_PRIM_RX;
    nrf24_write_register(config, NRF24_REG_CONFIG, cfg);

    gpio_pin_set(config->gpio_dev, config->ce_pin, 1);
    k_usleep(130); /* RX settling time */

    return 0;
}

int nrf24_tx_mode(struct nrf24_config *config)
{
    gpio_pin_set(config->gpio_dev, config->ce_pin, 0);

    uint8_t cfg = NRF24_CONFIG_EN_CRC | NRF24_CONFIG_CRCO | NRF24_CONFIG_PWR_UP;
    nrf24_write_register(config, NRF24_REG_CONFIG, cfg);

    return 0;
}

int nrf24_transmit(struct nrf24_config *config, const uint8_t *data, size_t len)
{
    if (len > 32)
    {
        return -EINVAL;
    }

    /* Switch to TX mode */
    nrf24_tx_mode(config);

    /* Flush TX FIFO */
    nrf24_send_command(config, NRF24_CMD_FLUSH_TX);

    /* Write payload */
    uint8_t tx_buf[33];
    tx_buf[0] = NRF24_CMD_W_TX_PAYLOAD;
    memcpy(&tx_buf[1], data, len);

    const struct spi_buf tx = {.buf = tx_buf, .len = len + 1};
    const struct spi_buf_set tx_bufs = {.buffers = &tx, .count = 1};
    spi_write(config->spi_dev, &config->spi_cfg, &tx_bufs);

    /* Pulse CE to start transmission */
    gpio_pin_set(config->gpio_dev, config->ce_pin, 1);
    k_usleep(15);
    gpio_pin_set(config->gpio_dev, config->ce_pin, 0);

    /* Wait for transmission */
    uint8_t status;
    int timeout = 100;
    while (timeout--)
    {
        nrf24_read_register(config, NRF24_REG_STATUS, &status);
        if (status & (NRF24_STATUS_TX_DS | NRF24_STATUS_MAX_RT))
        {
            break;
        }
        k_usleep(100);
    }

    /* Clear status */
    nrf24_write_register(config, NRF24_REG_STATUS, 0x70);

    if (status & NRF24_STATUS_MAX_RT)
    {
        LOG_WRN("Max retries reached");
        return -EAGAIN;
    }

    return 0;
}

int nrf24_receive(struct nrf24_config *config, uint8_t *data, size_t max_len)
{
    uint8_t status;
    nrf24_read_register(config, NRF24_REG_STATUS, &status);

    if (!(status & NRF24_STATUS_RX_DR))
    {
        return 0;
    }

    /* Read payload length */
    uint8_t len;
    nrf24_read_register(config, NRF24_REG_RX_PW_P0, &len);

    if (len > max_len || len > 32)
    {
        len = max_len < 32 ? max_len : 32;
    }

    /* Read payload */
    uint8_t tx_buf[33] = {NRF24_CMD_R_RX_PAYLOAD};
    uint8_t rx_buf[33];
    const struct spi_buf tx = {.buf = tx_buf, .len = len + 1};
    const struct spi_buf_set tx_bufs = {.buffers = &tx, .count = 1};
    const struct spi_buf rx = {.buf = rx_buf, .len = len + 1};
    const struct spi_buf_set rx_bufs = {.buffers = &rx, .count = 1};

    spi_transceive(config->spi_dev, &config->spi_cfg, &tx_bufs, &rx_bufs);
    memcpy(data, &rx_buf[1], len);

    /* Clear RX interrupt */
    nrf24_write_register(config, NRF24_REG_STATUS, NRF24_STATUS_RX_DR);

    return len;
}

bool nrf24_available(struct nrf24_config *config)
{
    uint8_t status;
    nrf24_read_register(config, NRF24_REG_STATUS, &status);
    return (status & NRF24_STATUS_RX_DR) != 0;
}

int nrf24_set_tx_address(struct nrf24_config *config, const uint8_t *address)
{
    return nrf24_write_register_multi(config, NRF24_REG_TX_ADDR,
                                      address, config->address_width);
}

int nrf24_set_rx_address(struct nrf24_config *config, uint8_t pipe, const uint8_t *address)
{
    if (pipe > 5)
    {
        return -EINVAL;
    }

    return nrf24_write_register_multi(config, NRF24_REG_RX_ADDR_P0 + pipe,
                                      address, config->address_width);
}

int nrf24_power_down(struct nrf24_config *config)
{
    gpio_pin_set(config->gpio_dev, config->ce_pin, 0);
    return nrf24_write_register(config, NRF24_REG_CONFIG, 0x00);
}

int nrf24_power_up(struct nrf24_config *config)
{
    uint8_t cfg = NRF24_CONFIG_EN_CRC | NRF24_CONFIG_CRCO | NRF24_CONFIG_PWR_UP;
    return nrf24_write_register(config, NRF24_REG_CONFIG, cfg);
}
