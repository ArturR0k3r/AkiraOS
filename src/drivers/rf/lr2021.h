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

#ifdef __cplusplus
}
#endif

#endif /* LR2021_H */
