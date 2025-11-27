/**
 * @file trust_levels.h
 * @brief AkiraOS Trust Level Definitions
 */

#ifndef AKIRA_TRUST_LEVELS_H
#define AKIRA_TRUST_LEVELS_H

/**
 * @brief Trust levels for apps and system components
 * 
 * Level 0 (KERNEL): Direct hardware access, all memory
 * Level 1 (SYSTEM): Privileged APIs, DMA access
 * Level 2 (TRUSTED): Extended permissions, signed apps
 * Level 3 (USER): Sandboxed, capability-based access
 */
typedef enum {
	TRUST_LEVEL_KERNEL = 0,   // Zephyr RTOS, HAL
	TRUST_LEVEL_SYSTEM = 1,   // OTA Manager, RF Manager
	TRUST_LEVEL_TRUSTED = 2,  // Signed apps, system utilities
	TRUST_LEVEL_USER = 3      // User WASM apps
} akira_trust_level_t;

/**
 * @brief Check if a trust level can access another
 * @param current Current trust level
 * @param required Required trust level
 * @return true if access is allowed
 */
static inline bool trust_level_allows(akira_trust_level_t current, 
                                       akira_trust_level_t required)
{
	return current <= required;
}

#endif /* AKIRA_TRUST_LEVELS_H */
