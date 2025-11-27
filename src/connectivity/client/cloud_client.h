/**
 * @file cloud_client.h
 * @brief Unified Cloud Client for AkiraOS
 *
 * Single communication layer for all cloud interactions:
 * - OTA firmware updates
 * - WASM app downloads and updates
 * - Push notifications and commands
 * - Data synchronization
 */

#ifndef AKIRA_CLOUD_CLIENT_H
#define AKIRA_CLOUD_CLIENT_H

#include "cloud_protocol.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*===========================================================================*/
    /* Configuration                                                             */
    /*===========================================================================*/

#define CLOUD_CLIENT_MAX_URL_LEN 256
#define CLOUD_CLIENT_MAX_HANDLERS 8
#define CLOUD_CLIENT_RX_BUFFER_SIZE 4096
#define CLOUD_CLIENT_TX_BUFFER_SIZE 1024
#define CLOUD_CLIENT_MAX_PENDING 16

    /*===========================================================================*/
    /* Types                                                                     */
    /*===========================================================================*/

    /** Cloud client states */
    typedef enum
    {
        CLOUD_STATE_DISCONNECTED = 0,
        CLOUD_STATE_CONNECTING,
        CLOUD_STATE_AUTHENTICATING,
        CLOUD_STATE_CONNECTED,
        CLOUD_STATE_RECONNECTING,
        CLOUD_STATE_ERROR
    } cloud_client_state_t;

    /** Transport types */
    typedef enum
    {
        CLOUD_TRANSPORT_WEBSOCKET = 0,
        CLOUD_TRANSPORT_COAP,
        CLOUD_TRANSPORT_MQTT,
        CLOUD_TRANSPORT_COUNT
    } cloud_transport_t;

    /** Connection configuration */
    typedef struct
    {
        const char *url;                 /**< Server URL */
        cloud_transport_t transport;     /**< Transport type */
        const char *device_id;           /**< Device identifier */
        const char *auth_token;          /**< Authentication token */
        bool auto_reconnect;             /**< Auto-reconnect on disconnect */
        uint32_t reconnect_delay_ms;     /**< Initial reconnect delay */
        uint32_t reconnect_max_delay_ms; /**< Maximum reconnect delay */
        uint32_t heartbeat_interval_ms;  /**< Heartbeat interval (0=disable) */
    } cloud_config_t;

/** Default configuration */
#define CLOUD_CONFIG_DEFAULT {              \
    .url = NULL,                            \
    .transport = CLOUD_TRANSPORT_WEBSOCKET, \
    .device_id = NULL,                      \
    .auth_token = NULL,                     \
    .auto_reconnect = true,                 \
    .reconnect_delay_ms = 1000,             \
    .reconnect_max_delay_ms = 60000,        \
    .heartbeat_interval_ms = 30000,         \
}

    /** Cloud events */
    typedef enum
    {
        CLOUD_EVENT_CONNECTED,
        CLOUD_EVENT_DISCONNECTED,
        CLOUD_EVENT_AUTH_SUCCESS,
        CLOUD_EVENT_AUTH_FAILED,
        CLOUD_EVENT_ERROR,
        CLOUD_EVENT_RECONNECTING,
    } cloud_event_t;

    /** Event callback */
    typedef void (*cloud_event_cb_t)(cloud_event_t event, void *user_data);

    /** Message handler callback
     * @param type Message type
     * @param payload Payload data
     * @param payload_len Payload length
     * @param msg_id Message ID for responses
     * @param user_data User data
     * @return 0 if handled, -ENOENT if not handled (try next handler)
     */
    typedef int (*cloud_msg_handler_t)(cloud_msg_type_t type,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint32_t msg_id,
                                       void *user_data);

    /** Download progress callback */
    typedef void (*cloud_download_progress_cb_t)(const char *id,
                                                 size_t received,
                                                 size_t total,
                                                 void *user_data);

    /** Download complete callback */
    typedef void (*cloud_download_complete_cb_t)(const char *id,
                                                 bool success,
                                                 const uint8_t *data,
                                                 size_t size,
                                                 void *user_data);

    /*===========================================================================*/
    /* Initialization                                                            */
    /*===========================================================================*/

    /**
     * @brief Initialize cloud client
     * @return 0 on success
     */
    int cloud_client_init(void);

    /**
     * @brief Deinitialize cloud client
     */
    void cloud_client_deinit(void);

    /*===========================================================================*/
    /* Connection Management                                                     */
    /*===========================================================================*/

    /**
     * @brief Connect to cloud server
     * @param config Connection configuration
     * @return 0 on success
     */
    int cloud_client_connect(const cloud_config_t *config);

    /**
     * @brief Disconnect from cloud server
     * @return 0 on success
     */
    int cloud_client_disconnect(void);

    /**
     * @brief Get current connection state
     * @return Current state
     */
    cloud_client_state_t cloud_client_get_state(void);

    /**
     * @brief Check if connected
     * @return true if connected and authenticated
     */
    bool cloud_client_is_connected(void);

    /*===========================================================================*/
    /* Event and Message Handlers                                                */
    /*===========================================================================*/

    /**
     * @brief Register event callback
     * @param callback Event callback
     * @param user_data User data for callback
     */
    void cloud_client_set_event_cb(cloud_event_cb_t callback, void *user_data);

    /**
     * @brief Register message handler
     * @param category Message category to handle (or 0xFF for all)
     * @param handler Handler callback
     * @param user_data User data for callback
     * @return Handler ID (>= 0) or negative error
     */
    int cloud_client_register_handler(cloud_msg_category_t category,
                                      cloud_msg_handler_t handler,
                                      void *user_data);

    /**
     * @brief Unregister message handler
     * @param handler_id Handler ID
     */
    void cloud_client_unregister_handler(int handler_id);

    /*===========================================================================*/
    /* Message Sending                                                           */
    /*===========================================================================*/

    /**
     * @brief Send message to cloud
     * @param type Message type
     * @param payload Payload data
     * @param payload_len Payload length
     * @return Message ID (> 0) or negative error
     */
    int cloud_client_send(cloud_msg_type_t type,
                          const void *payload,
                          size_t payload_len);

    /**
     * @brief Send response to a received message
     * @param msg_id Original message ID
     * @param type Response message type
     * @param payload Response payload
     * @param payload_len Payload length
     * @return 0 on success
     */
    int cloud_client_respond(uint32_t msg_id,
                             cloud_msg_type_t type,
                             const void *payload,
                             size_t payload_len);

    /*===========================================================================*/
    /* Status Reporting                                                          */
    /*===========================================================================*/

    /**
     * @brief Send device status to cloud
     * @param status Status structure (NULL to auto-fill)
     * @return 0 on success
     */
    int cloud_client_send_status(const cloud_device_status_t *status);

    /*===========================================================================*/
    /* OTA Integration                                                           */
    /*===========================================================================*/

    /**
     * @brief Check for firmware updates
     * @return 0 on success (result delivered via handler)
     */
    int cloud_client_check_firmware(void);

    /**
     * @brief Request firmware download
     * @param version Version to download (NULL for latest)
     * @param progress_cb Progress callback
     * @param complete_cb Completion callback
     * @param user_data User data for callbacks
     * @return 0 on success
     */
    int cloud_client_download_firmware(const char *version,
                                       cloud_download_progress_cb_t progress_cb,
                                       cloud_download_complete_cb_t complete_cb,
                                       void *user_data);

    /*===========================================================================*/
    /* App Management Integration                                                */
    /*===========================================================================*/

    /**
     * @brief Request app catalog from cloud
     * @return 0 on success (result delivered via handler)
     */
    int cloud_client_request_app_list(void);

    /**
     * @brief Check for app updates
     * @return 0 on success (result delivered via handler)
     */
    int cloud_client_check_app_updates(void);

    /**
     * @brief Download app from cloud
     * @param app_id App identifier
     * @param progress_cb Progress callback
     * @param complete_cb Completion callback
     * @param user_data User data for callbacks
     * @return 0 on success
     */
    int cloud_client_download_app(const char *app_id,
                                  cloud_download_progress_cb_t progress_cb,
                                  cloud_download_complete_cb_t complete_cb,
                                  void *user_data);

    /**
     * @brief Report installed apps to cloud
     * @return 0 on success
     */
    int cloud_client_report_apps(void);

    /*===========================================================================*/
    /* Statistics                                                                */
    /*===========================================================================*/

    /** Cloud client statistics */
    typedef struct
    {
        uint32_t messages_sent;
        uint32_t messages_received;
        uint64_t bytes_sent;
        uint64_t bytes_received;
        uint32_t reconnect_count;
        uint32_t error_count;
        uint32_t connected_time_sec;
    } cloud_client_stats_t;

    /**
     * @brief Get client statistics
     * @param stats Output statistics
     */
    void cloud_client_get_stats(cloud_client_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CLOUD_CLIENT_H */
