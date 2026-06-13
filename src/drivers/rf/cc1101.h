/**
 * @file cc1101.h
 * @brief CC1101 Sub-GHz Transceiver Driver
 * @stability experimental
 * @since 1.4
 */

#ifndef AKIRA_CC1101_H
#define AKIRA_CC1101_H

#include "connectivity/radio_interface.h"
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cc1101_config {
    const struct device *spi_dev;
    uint32_t spi_freq;
    uint8_t cs_pin;
    uint8_t gdo0_pin;
    uint8_t gdo2_pin;
};

/** @brief Initialize CC1101 driver with config */
int cc1101_init_with_config(const struct cc1101_config *config);

/** @brief Get the CC1101 radio handle */
radio_handle_t *cc1101_get_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CC1101_H */
