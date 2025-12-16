/**
 * @file hardware_manager.h
 * @brief Hardware Subsystem Manager
 * 
 * Coordinates hardware initialization using the HAL and driver registry.
 */

#ifndef AKIRA_HARDWARE_MANAGER_H
#define AKIRA_HARDWARE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the hardware manager
 * 
 * This will:
 * - Initialize the HAL
 * - Initialize driver registry
 * - Load configured drivers
 * - Initialize button handlers
 * - Initialize display (if configured)
 * 
 * @return 0 on success, negative errno on failure
 */
int hardware_manager_init(void);

/**
 * @brief Get hardware initialization status
 * 
 * @return true if initialized, false otherwise
 */
bool hardware_manager_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_HARDWARE_MANAGER_H */
