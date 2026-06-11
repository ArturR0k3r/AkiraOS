/**
 * @file cc1121.h
 * @brief CC1121RHB Sub-GHz RF Transceiver Driver
 *
 * TI CC1121: high-performance narrow-band Sub-GHz transceiver.
 * Covers 315/433/868/915 MHz bands via SPI (max 10 MHz).
 * Registers in the CC112x family.
 * @stability experimental
 * @since 1.5
 */

#ifndef AKIRA_CC1121_H
#define AKIRA_CC1121_H

#include "rf_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Get CC1121 driver interface (RF framework registration). */
const struct akira_rf_driver *cc1121_get_driver(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CC1121_H */
