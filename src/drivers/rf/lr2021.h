/**
 * @file lr2021.h
 * @brief LR2021 LoRa Plus Transceiver Driver Interface
 */

#ifndef LR2021_H
#define LR2021_H

#include "rf_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Get the LR2021 driver instance */
const struct akira_rf_driver *lr2021_get_driver(void);

/** @brief Get the LR2021 radio_handle_t (RADIO_MANAGER path) */
struct radio_handle *lr2021_get_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* LR2021_H */
