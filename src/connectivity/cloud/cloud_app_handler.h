/**
 * @file cloud_app_handler.h
 * @brief Cloud App Handler for AkiraOS
 *
 * Handles app-related messages from cloud/BT/web:
 * - App catalog requests
 * - App downloads (new apps and updates)
 * - App management commands (install, start, stop, uninstall)
 *
 * Integrates with app_loader and wasm_app_manager.
 */

#ifndef AKIRA_CLOUD_APP_HANDLER_H
#define AKIRA_CLOUD_APP_HANDLER_H

#include "cloud_protocol.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define APP_DOWNLOAD_BUFFER_SIZE    (64 * 1024)  /**< 64KB download buffer */
#define APP_MAX_PENDING_DOWNLOADS   2

/*===========================================================================*/
/* Types                                                                     */
/*===========================================================================*/

/** App download state */
typedef enum {
    APP_DL_IDLE = 0,
    APP_DL_METADATA,
    APP_DL_RECEIVING,
    APP_DL_VERIFYING,
    APP_DL_INSTALLING,
    APP_DL_COMPLETE,
    APP_DL_ERROR
} app_download_state_t;

/** App download progress callback */
typedef void (*app_download_progress_cb_t)(const char *app_id,
                                            uint32_t received,
                                            uint32_t total,
                                            void *user_data);

/** App download complete callback */
typedef void (*app_download_complete_cb_t)(const char *app_id,
                                            bool success,
                                            const char *error,
                                            void *user_data);

/** Download request */
typedef struct {
    char app_id[32];
    app_download_progress_cb_t progress_cb;
    app_download_complete_cb_t complete_cb;
    void *user_data;
    bool auto_install;
    bool auto_start;
} app_download_request_t;

/*===========================================================================*/
/* Initialization                                                            */
/*===========================================================================*/

/**
 * @brief Initialize app handler
 * @return 0 on success
 */
int cloud_app_handler_init(void);

/**
 * @brief Deinitialize app handler
 * @return 0 on success
 */
int cloud_app_handler_deinit(void);

/*===========================================================================*/
/* App Download                                                              */
/*===========================================================================*/

/**
 * @brief Start app download
 * @param request Download request
 * @return 0 on success
 */
int cloud_app_download(const app_download_request_t *request);

/**
 * @brief Cancel app download
 * @param app_id App to cancel (NULL for all)
 * @return 0 on success
 */
int cloud_app_cancel_download(const char *app_id);

/**
 * @brief Get download state
 * @param app_id App ID
 * @return Current state
 */
app_download_state_t cloud_app_get_download_state(const char *app_id);

/**
 * @brief Get download progress
 * @param app_id App ID
 * @param received Output: bytes received
 * @param total Output: total bytes
 * @return 0 on success
 */
int cloud_app_get_download_progress(const char *app_id,
                                     uint32_t *received,
                                     uint32_t *total);

/*===========================================================================*/
/* App Updates                                                               */
/*===========================================================================*/

/**
 * @brief Check for updates for installed apps
 * @return Number of updates available or negative error
 */
int cloud_app_check_updates(void);

/**
 * @brief Update a specific app
 * @param app_id App to update (NULL for all)
 * @param complete_cb Callback when done
 * @param user_data User data for callback
 * @return 0 on success
 */
int cloud_app_update(const char *app_id,
                      app_download_complete_cb_t complete_cb,
                      void *user_data);

/*===========================================================================*/
/* App Catalog                                                               */
/*===========================================================================*/

/** App catalog entry */
typedef struct {
    char app_id[32];
    char name[32];
    char description[128];
    uint8_t version[4];
    uint32_t size;
    uint64_t permissions;
    bool installed;
    bool has_update;
} app_catalog_entry_t;

/** App catalog callback */
typedef void (*app_catalog_cb_t)(const app_catalog_entry_t *entries,
                                  int count, void *user_data);

/**
 * @brief Request app catalog from cloud
 * @param callback Callback with results
 * @param user_data User data for callback
 * @return 0 on success
 */
int cloud_app_request_catalog(app_catalog_cb_t callback, void *user_data);

/*===========================================================================*/
/* Internal Message Handler                                                  */
/*===========================================================================*/

/**
 * @brief Handle incoming app message (called by cloud_client)
 * @param msg Message
 * @param source Message source
 * @return 0 if handled
 */
int cloud_app_handle_message(const cloud_message_t *msg, msg_source_t source);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CLOUD_APP_HANDLER_H */
