/**
 * @file cc1121.h
 * @brief CC1121RHB Sub-GHz RF Transceiver Driver
 *
 * TI CC1121: high-performance narrow-band Sub-GHz transceiver.
 * Covers 315/433/868/915 MHz bands via SPI (max 10 MHz).
 * @stability experimental
 * @since 1.5
 */

#ifndef AKIRA_CC1121_H
#define AKIRA_CC1121_H

#include "connectivity/radio_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Get the CC1121 radio handle */
radio_handle_t *cc1121_get_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CC1121_H */
