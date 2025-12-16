/**
 * @file system_manager.h
 * @brief Main System Manager for AkiraOS
 * 
 * Orchestrates the initialization of all subsystems using the init table
 * and coordinates the runtime system.
 */

#ifndef AKIRA_SYSTEM_MANAGER_H
#define AKIRA_SYSTEM_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the system manager and all subsystems
 * 
 * This is the main entry point that orchestrates:
 * - Event bus initialization
 * - Subsystem registration
 * - Priority-based initialization
 * - System ready signal
 * 
 * @return 0 on success, negative errno on failure
 */
int system_manager_init(void);

/**
 * @brief Start the system manager runtime loop
 * 
 * This starts background services and monitoring.
 * Call this after system_manager_init() completes.
 * 
 * @return 0 on success, negative errno on failure
 */
int system_manager_start(void);

/**
 * @brief Get system initialization status
 * 
 * @return true if fully initialized, false otherwise
 */
bool system_manager_is_ready(void);

/**
 * @brief Shutdown the system gracefully
 * 
 * @return 0 on success, negative errno on failure
 */
int system_manager_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SYSTEM_MANAGER_H */
