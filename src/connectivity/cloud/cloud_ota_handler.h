/**
 * @file cloud_ota_handler.h
 * @brief Cloud OTA Handler for AkiraOS
 *
 * Handles firmware update messages from any source:
 * - Cloud server (WebSocket)
 * - AkiraApp (Bluetooth)
 * - Local web server
 *
 * Integrates with OTA manager for flashing.
 */

#ifndef AKIRA_CLOUD_OTA_HANDLER_H
#define AKIRA_CLOUD_OTA_HANDLER_H

#include "cloud_protocol.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* Types                                                                     */
/*===========================================================================*/

/** OTA download state */
typedef enum {
    OTA_DL_IDLE = 0,
    OTA_DL_CHECKING,
    OTA_DL_AVAILABLE,
    OTA_DL_RECEIVING,
    OTA_DL_VERIFYING,
    OTA_DL_READY,
    OTA_DL_APPLYING,
    OTA_DL_ERROR
} ota_download_state_t;

/** Firmware info */
typedef struct {
    uint8_t version[4];     /**< Major, Minor, Patch, Build */
    uint32_t size;          /**< Total size */
    uint8_t hash[32];       /**< SHA-256 hash */
    char release_notes[128];
    msg_source_t source;    /**< Where it came from */
} ota_firmware_info_t;

/** OTA progress callback */
typedef void (*cloud_ota_progress_cb_t)(uint32_t received, uint32_t total, void *user_data);

/** OTA complete callback */
typedef void (*cloud_ota_complete_cb_t)(bool success, const char *error, void *user_data);

/** OTA available callback */
typedef void (*cloud_ota_available_cb_t)(const ota_firmware_info_t *info, void *user_data);

/*===========================================================================*/
/* Initialization                                                            */
/*===========================================================================*/

/**
 * @brief Initialize OTA handler
 * @return 0 on success
 */
int cloud_ota_handler_init(void);

/**
 * @brief Deinitialize OTA handler
 * @return 0 on success
 */
int cloud_ota_handler_deinit(void);

/*===========================================================================*/
/* OTA Operations                                                            */
/*===========================================================================*/

/**
 * @brief Check for firmware updates
 * @param callback Called when update available (or NULL for none)
 * @param user_data User data for callback
 * @return 0 on success
 */
int cloud_ota_check(cloud_ota_available_cb_t callback, void *user_data);

/**
 * @brief Start firmware download
 * @param version Version to download (NULL for latest)
 * @param progress_cb Progress callback
 * @param complete_cb Completion callback
 * @param user_data User data for callbacks
 * @return 0 on success
 */
int cloud_ota_download(const char *version,
                        cloud_ota_progress_cb_t progress_cb,
                        cloud_ota_complete_cb_t complete_cb,
                        void *user_data);

/**
 * @brief Cancel ongoing download
 * @return 0 on success
 */
int cloud_ota_cancel(void);

/**
 * @brief Apply downloaded firmware (triggers reboot)
 * @return 0 on success (won't return on success)
 */
int cloud_ota_apply(void);

/**
 * @brief Get current OTA state
 * @return Current state
 */
ota_download_state_t cloud_ota_get_state(void);

/**
 * @brief Get download progress
 * @param received Output: bytes received
 * @param total Output: total bytes
 * @return 0 on success
 */
int cloud_ota_get_progress(uint32_t *received, uint32_t *total);

/**
 * @brief Get available firmware info
 * @param info Output: firmware info
 * @return 0 if available
 */
int cloud_ota_get_available_info(ota_firmware_info_t *info);

/*===========================================================================*/
/* Internal Message Handler                                                  */
/*===========================================================================*/

/**
 * @brief Handle incoming OTA message (called by cloud_client)
 * @param msg Message
 * @param source Message source
 * @return 0 if handled
 */
int cloud_ota_handle_message(const cloud_message_t *msg, msg_source_t source);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CLOUD_OTA_HANDLER_H */
