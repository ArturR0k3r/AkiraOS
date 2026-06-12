/**
 * @file lr1121.h
 * @brief LR1121 LoRa/GFSK Transceiver Driver
 * @stability experimental
 * @since 1.4
 */

#ifndef AKIRA_LR1121_H
#define AKIRA_LR1121_H

#include "connectivity/radio_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Get the LR1121 radio handle */
radio_handle_t *lr1121_get_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_LR1121_H */
