/**
 * @file lr1121.h
 * @brief LR1121 LoRa/GFSK Transceiver Driver
 */

#ifndef AKIRA_LR1121_H
#define AKIRA_LR1121_H

#include "rf_framework.h"
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LR1121 configuration
 */
struct lr1121_config {
	const struct device *spi_dev;
	uint32_t spi_freq;
	uint8_t cs_pin;
	uint8_t reset_pin;
	uint8_t busy_pin;
	uint8_t irq_pin;
};

/**
 * @brief Initialize LR1121 driver
 * @param config Hardware configuration
 * @return 0 on success
 */
int lr1121_init_with_config(const struct lr1121_config *config);

/**
 * @brief Get LR1121 driver interface
 * @return Driver interface pointer
 */
const struct akira_rf_driver *lr1121_get_driver(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_LR1121_H */
