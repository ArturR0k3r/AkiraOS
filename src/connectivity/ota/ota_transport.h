/**
 * @file ota_transport.h
 * @brief OTA Transport Interface for AkiraOS
 *
 * Defines the common interface for OTA update sources:
 * - HTTP (web server upload)
 * - Bluetooth (BLE OTA)
 * - USB (USB OTA)
 * - Cloud (future AkiraHub)
 */

#ifndef AKIRA_OTA_TRANSPORT_H
#define AKIRA_OTA_TRANSPORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*===========================================================================*/
    /* OTA Source Types                                                          */
    /*===========================================================================*/

    /** Available OTA sources */
    typedef enum
    {
        OTA_SOURCE_NONE = 0x00,
        OTA_SOURCE_HTTP = 0x01,  /**< HTTP/Web server upload */
        OTA_SOURCE_BLE = 0x02,   /**< Bluetooth Low Energy */
        OTA_SOURCE_USB = 0x04,   /**< USB connection */
        OTA_SOURCE_CLOUD = 0x08, /**< Cloud (AkiraHub) */
        OTA_SOURCE_ALL = 0x0F
    } ota_source_t;

    /** OTA transport states */
    typedef enum
    {
        OTA_TRANSPORT_IDLE = 0,
        OTA_TRANSPORT_READY,
        OTA_TRANSPORT_RECEIVING,
        OTA_TRANSPORT_ERROR
    } ota_transport_state_t;

    /*===========================================================================*/
    /* Transport Callbacks                                                       */
    /*===========================================================================*/

    /**
     * @brief Called when firmware data chunk is received
     * @param data Chunk data
     * @param len Chunk length
     * @param offset Offset in firmware
     * @param total Total firmware size (0 if unknown)
     * @return 0 on success
     */
    typedef int (*ota_data_cb_t)(const uint8_t *data, size_t len,
                                 size_t offset, size_t total);

    /**
     * @brief Called when OTA transfer completes or fails
     * @param success true if completed successfully
     * @param error_msg Error message (NULL on success)
     */
    typedef void (*ota_transport_complete_cb_t)(bool success, const char *error_msg);

    /**
     * @brief Called to report progress (simple version for transports)
     * @param percent Progress percentage (0-100)
     */
    typedef void (*ota_transport_progress_cb_t)(uint8_t percent);

    /*===========================================================================*/
    /* Transport Operations                                                      */
    /*===========================================================================*/

    /** OTA transport operations */
    typedef struct
    {
        const char *name;
        ota_source_t source;

        /**
         * @brief Initialize transport
         * @return 0 on success
         */
        int (*init)(void);

        /**
         * @brief Deinitialize transport
         * @return 0 on success
         */
        int (*deinit)(void);

        /**
         * @brief Enable transport (start listening for updates)
         * @return 0 on success
         */
        int (*enable)(void);

        /**
         * @brief Disable transport
         * @return 0 on success
         */
        int (*disable)(void);

        /**
         * @brief Check if transport is available
         * @return true if available
         */
        bool (*is_available)(void);

        /**
         * @brief Check if transport is active (receiving)
         * @return true if active
         */
        bool (*is_active)(void);

        /**
         * @brief Abort current transfer
         * @return 0 on success
         */
        int (*abort)(void);

        /**
         * @brief Get transport state
         * @return Current state
         */
        ota_transport_state_t (*get_state)(void);

    } ota_transport_ops_t;

    /*===========================================================================*/
    /* Transport Registration API                                                */
    /*===========================================================================*/

    /**
     * @brief Register OTA transport
     * @param ops Transport operations
     * @return 0 on success
     */
    int ota_transport_register(const ota_transport_ops_t *ops);

    /**
     * @brief Unregister OTA transport
     * @param source Source type
     * @return 0 on success
     */
    int ota_transport_unregister(ota_source_t source);

    /**
     * @brief Get registered transport
     * @param source Source type
     * @return Transport ops or NULL
     */
    const ota_transport_ops_t *ota_transport_get(ota_source_t source);

    /**
     * @brief Set data callback (called by transports)
     * @param callback Data callback
     */
    void ota_transport_set_data_cb(ota_data_cb_t callback);

    /**
     * @brief Set completion callback
     * @param callback Completion callback
     */
    void ota_transport_set_complete_cb(ota_transport_complete_cb_t callback);

    /**
     * @brief Set progress callback
     * @param callback Progress callback
     */
    void ota_transport_set_progress_cb(ota_transport_progress_cb_t callback);

    /**
     * @brief Get available sources (enabled transports)
     * @return Bitmask of available sources
     */
    ota_source_t ota_transport_get_available(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_OTA_TRANSPORT_H */
