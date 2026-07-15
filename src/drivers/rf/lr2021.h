/**
 * @file lr2021.h
 * @brief LR2021 LoRa Plus Transceiver Driver Interface
 */

#ifndef LR2021_H
#define LR2021_H

#include "connectivity/radio_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Get the LR2021 radio handle */
radio_handle_t *lr2021_get_handle(void);

/** @brief Key a continuous-wave carrier at the current frequency+power.
 *  Chip stays in TX CW until lr2021_tx_cw_stop() is called. */
int lr2021_tx_cw_start(void);

/** @brief Stop a CW carrier and return to standby. */
int lr2021_tx_cw_stop(void);

/** @brief Fast frequency change while CW is active — PLL lock only, no CalibFe.
 *  ~1ms vs ~20ms for lr2021_set_frequency(). Caller must have set initial
 *  frequency+power via the normal path first. */
int lr2021_tx_cw_set_freq_fast(uint32_t freq_hz);

#ifdef __cplusplus
}
#endif

#endif /* LR2021_H */
