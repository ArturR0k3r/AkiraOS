/**
 * @file cc1101.h
 * @brief CC1101 Sub-GHz Transceiver Driver
 */

#ifndef AKIRA_CC1101_H
#define AKIRA_CC1101_H

#include "rf_framework.h"
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief CC1101 configuration
     */
    struct cc1101_config
    {
        const struct device *spi_dev;
        uint32_t spi_freq;
        uint8_t cs_pin;
        uint8_t gdo0_pin;
        uint8_t gdo2_pin;
    };

    /**
     * @brief Initialize CC1101 driver
     * @param config Hardware configuration
     * @return 0 on success
     */
    int cc1101_init_with_config(const struct cc1101_config *config);

    /**
     * @brief Get CC1101 driver interface
     * @return Driver interface pointer
     */
    const struct akira_rf_driver *cc1101_get_driver(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CC1101_H */
