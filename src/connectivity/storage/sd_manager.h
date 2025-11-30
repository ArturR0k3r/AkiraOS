/**
 * @file sd_manager.h
 * @brief SD Card Manager for App Discovery
 *
 * Handles SD card mounting and WASM app scanning.
 */

#ifndef AKIRA_SD_MANAGER_H
#define AKIRA_SD_MANAGER_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MOUNT_POINT    "/sd"
#define SD_APPS_DIR       "/sd/apps"

/**
 * @brief SD card state
 */
typedef enum {
    SD_STATE_UNMOUNTED = 0,
    SD_STATE_MOUNTED,
    SD_STATE_ERROR,
} sd_state_t;

/**
 * @brief SD card event callback
 */
typedef void (*sd_event_cb_t)(sd_state_t state, void *user_data);

/**
 * @brief Initialize SD card manager
 *
 * @return 0 on success, negative on error
 */
int sd_manager_init(void);

/**
 * @brief Mount SD card
 *
 * @return 0 on success, negative on error
 */
int sd_manager_mount(void);

/**
 * @brief Unmount SD card
 *
 * @return 0 on success, negative on error
 */
int sd_manager_unmount(void);

/**
 * @brief Check if SD card is mounted
 *
 * @return true if mounted
 */
bool sd_manager_is_mounted(void);

/**
 * @brief Get SD card state
 *
 * @return Current state
 */
sd_state_t sd_manager_get_state(void);

/**
 * @brief Scan SD card for WASM apps
 *
 * Scans SD_APPS_DIR for *.wasm files.
 *
 * @param names Output array of app names
 * @param max_count Maximum names to return
 * @return Number of apps found, negative on error
 */
int sd_manager_scan_apps(char names[][32], int max_count);

/**
 * @brief Install app from SD card
 *
 * @param name App name (without .wasm extension)
 * @return App ID on success, negative on error
 */
int sd_manager_install_app(const char *name);

/**
 * @brief Install all apps from SD card
 *
 * @return Number of apps installed, negative on error
 */
int sd_manager_install_all_apps(void);

/**
 * @brief Register event callback
 *
 * @param callback Callback function
 * @param user_data User data
 */
void sd_manager_register_callback(sd_event_cb_t callback, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SD_MANAGER_H */
