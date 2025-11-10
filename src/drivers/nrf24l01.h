/*
 * SPDX-License-Identifier: Apache-2.0
 * 
 * NRF24L01+ Wireless Transceiver Driver for AkiraOS
 */

#ifndef NRF24L01_H
#define NRF24L01_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NRF24L01+ Register Map */
#define NRF24_REG_CONFIG      0x00
#define NRF24_REG_EN_AA       0x01
#define NRF24_REG_EN_RXADDR   0x02
#define NRF24_REG_SETUP_AW    0x03
#define NRF24_REG_SETUP_RETR  0x04
#define NRF24_REG_RF_CH       0x05
#define NRF24_REG_RF_SETUP    0x06
#define NRF24_REG_STATUS      0x07
#define NRF24_REG_OBSERVE_TX  0x08
#define NRF24_REG_RPD         0x09
#define NRF24_REG_RX_ADDR_P0  0x0A
#define NRF24_REG_RX_ADDR_P1  0x0B
#define NRF24_REG_TX_ADDR     0x10
#define NRF24_REG_RX_PW_P0    0x11
#define NRF24_REG_FIFO_STATUS 0x17
#define NRF24_REG_DYNPD       0x1C
#define NRF24_REG_FEATURE     0x1D

/* Commands */
#define NRF24_CMD_R_REGISTER    0x00
#define NRF24_CMD_W_REGISTER    0x20
#define NRF24_CMD_R_RX_PAYLOAD  0x61
#define NRF24_CMD_W_TX_PAYLOAD  0xA0
#define NRF24_CMD_FLUSH_TX      0xE1
#define NRF24_CMD_FLUSH_RX      0xE2
#define NRF24_CMD_REUSE_TX_PL   0xE3
#define NRF24_CMD_NOP           0xFF

/* Configuration bits */
#define NRF24_CONFIG_MASK_RX_DR  0x40
#define NRF24_CONFIG_MASK_TX_DS  0x20
#define NRF24_CONFIG_MASK_MAX_RT 0x10
#define NRF24_CONFIG_EN_CRC      0x08
#define NRF24_CONFIG_CRCO        0x04
#define NRF24_CONFIG_PWR_UP      0x02
#define NRF24_CONFIG_PRIM_RX     0x01

/* Status bits */
#define NRF24_STATUS_RX_DR       0x40
#define NRF24_STATUS_TX_DS       0x20
#define NRF24_STATUS_MAX_RT      0x10
#define NRF24_STATUS_TX_FULL     0x01

/* Data rate */
enum nrf24_data_rate {
	NRF24_DATA_RATE_1MBPS,
	NRF24_DATA_RATE_2MBPS,
	NRF24_DATA_RATE_250KBPS
};

/* Power level */
enum nrf24_power {
	NRF24_POWER_0DBM,
	NRF24_POWER_MINUS_6DBM,
	NRF24_POWER_MINUS_12DBM,
	NRF24_POWER_MINUS_18DBM
};

/* Configuration structure */
struct nrf24_config {
	const struct device *spi_dev;
	struct spi_config spi_cfg;
	const struct device *gpio_dev;
	gpio_pin_t ce_pin;
	gpio_pin_t irq_pin;
	uint8_t channel;
	enum nrf24_data_rate data_rate;
	enum nrf24_power power;
	uint8_t address_width;
};

/**
 * @brief Initialize NRF24L01+ module
 * 
 * @param config Configuration structure
 * @return 0 on success, negative errno on failure
 */
int nrf24_init(struct nrf24_config *config);

/**
 * @brief Set RX mode
 * 
 * @param config Configuration structure
 * @return 0 on success, negative errno on failure
 */
int nrf24_rx_mode(struct nrf24_config *config);

/**
 * @brief Set TX mode
 * 
 * @param config Configuration structure
 * @return 0 on success, negative errno on failure
 */
int nrf24_tx_mode(struct nrf24_config *config);

/**
 * @brief Transmit data
 * 
 * @param config Configuration structure
 * @param data Data buffer
 * @param len Data length (max 32 bytes)
 * @return 0 on success, negative errno on failure
 */
int nrf24_transmit(struct nrf24_config *config, const uint8_t *data, size_t len);

/**
 * @brief Receive data
 * 
 * @param config Configuration structure
 * @param data Data buffer
 * @param max_len Maximum buffer length
 * @return Number of bytes received, negative errno on failure
 */
int nrf24_receive(struct nrf24_config *config, uint8_t *data, size_t max_len);

/**
 * @brief Check if data is available
 * 
 * @param config Configuration structure
 * @return true if data available, false otherwise
 */
bool nrf24_available(struct nrf24_config *config);

/**
 * @brief Set TX address
 * 
 * @param config Configuration structure
 * @param address Address buffer (5 bytes)
 * @return 0 on success, negative errno on failure
 */
int nrf24_set_tx_address(struct nrf24_config *config, const uint8_t *address);

/**
 * @brief Set RX address for pipe
 * 
 * @param config Configuration structure
 * @param pipe Pipe number (0-5)
 * @param address Address buffer (5 bytes)
 * @return 0 on success, negative errno on failure
 */
int nrf24_set_rx_address(struct nrf24_config *config, uint8_t pipe, const uint8_t *address);

/**
 * @brief Power down the module
 * 
 * @param config Configuration structure
 * @return 0 on success, negative errno on failure
 */
int nrf24_power_down(struct nrf24_config *config);

/**
 * @brief Power up the module
 * 
 * @param config Configuration structure
 * @return 0 on success, negative errno on failure
 */
int nrf24_power_up(struct nrf24_config *config);

#ifdef __cplusplus
}
#endif

#endif /* NRF24L01_H */
