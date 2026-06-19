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
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Get CC1121 driver interface (RF framework registration). */
const struct akira_rf_driver *cc1121_get_driver(void);

/** @brief Get the CC1121 radio_handle_t (RADIO_MANAGER path) */
struct radio_handle *cc1121_get_handle(void);

/** @brief Direct register write (for calibration / custom config). */
int cc1121_write_reg(uint8_t addr, uint8_t value);

/** @brief Direct register read. */
int cc1121_read_reg(uint8_t addr, uint8_t *value);

/** @brief Direct extended register write (addr ≥ 0x2F00). */
int cc1121_write_ext_reg(uint8_t ext_addr, uint8_t value);

/** @brief Direct extended register read. */
int cc1121_read_ext_reg(uint8_t ext_addr, uint8_t *value);

/** @brief Read MARCSTATE (current radio state machine value). */
int cc1121_get_marcstate(uint8_t *state);

/** @brief Is device initialized and ready. */
bool cc1121_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CC1121_H */
