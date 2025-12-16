/**
 * @file network_manager.h
 * @brief Network Subsystem Manager
 * 
 * Coordinates network connectivity (WiFi, Bluetooth, USB).
 */

#ifndef AKIRA_NETWORK_MANAGER_H
#define AKIRA_NETWORK_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the network manager
 * 
 * This will:
 * - Initialize WiFi (if configured)
 * - Initialize Bluetooth (if configured)
 * - Initialize USB (if configured)
 * - Start connection procedures based on settings
 * 
 * @return 0 on success, negative errno on failure
 */
int network_manager_init(void);

/**
 * @brief Get network initialization status
 * 
 * @return true if initialized, false otherwise
 */
bool network_manager_is_ready(void);

/**
 * @brief Check if any network interface is connected
 * 
 * @return true if connected, false otherwise
 */
bool network_manager_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_NETWORK_MANAGER_H */
