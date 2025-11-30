/**
 * @file usb_storage.h
 * @brief USB Mass Storage Manager for App Discovery
 *
 * Handles USB storage mounting and WASM app scanning.
 */

#ifndef AKIRA_USB_STORAGE_H
#define AKIRA_USB_STORAGE_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_MOUNT_POINT   "/usb"
#define USB_APPS_DIR      "/usb/apps"

/**
 * @brief USB storage state
 */
typedef enum {
    USB_STORAGE_DISCONNECTED = 0,
    USB_STORAGE_CONNECTED,
    USB_STORAGE_MOUNTED,
    USB_STORAGE_ERROR,
} usb_storage_state_t;

/**
 * @brief USB storage event callback
 */
typedef void (*usb_storage_event_cb_t)(usb_storage_state_t state, void *user_data);

/**
 * @brief Initialize USB storage manager
 *
 * @return 0 on success, negative on error
 */
int usb_storage_init(void);

/**
 * @brief Check if USB storage is mounted
 *
 * @return true if mounted
 */
bool usb_storage_is_mounted(void);

/**
 * @brief Get USB storage state
 *
 * @return Current state
 */
usb_storage_state_t usb_storage_get_state(void);

/**
 * @brief Scan USB storage for WASM apps
 *
 * Scans USB_APPS_DIR for *.wasm files.
 *
 * @param names Output array of app names
 * @param max_count Maximum names to return
 * @return Number of apps found, negative on error
 */
int usb_storage_scan_apps(char names[][32], int max_count);

/**
 * @brief Install app from USB storage
 *
 * @param name App name (without .wasm extension)
 * @return App ID on success, negative on error
 */
int usb_storage_install_app(const char *name);

/**
 * @brief Register event callback
 *
 * @param callback Callback function
 * @param user_data User data
 */
void usb_storage_register_callback(usb_storage_event_cb_t callback, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_USB_STORAGE_H */
