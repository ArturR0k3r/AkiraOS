/**
 * @file cloud_client.h
 * @brief Unified Cloud Client for AkiraOS
 *
 * Provides a unified interface for communication with:
 * - Remote cloud servers (WebSocket/CoAP/MQTT)
 * - AkiraApp mobile application (Bluetooth)
 * - Local web server
 *
 * All sources use the same message protocol and handler system.
 */

#ifndef AKIRA_CLOUD_CLIENT_H
#define AKIRA_CLOUD_CLIENT_H

#include "cloud_protocol.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define CLOUD_CLIENT_MAX_HANDLERS       8
#define CLOUD_CLIENT_MAX_SOURCES        4
#define CLOUD_CLIENT_RX_QUEUE_SIZE      16
#define CLOUD_CLIENT_TX_QUEUE_SIZE      16
#define CLOUD_CLIENT_CHUNK_SIZE         4096

/*===========================================================================*/
/* Types                                                                     */
/*===========================================================================*/

/** Connection state */
typedef enum {
    CLOUD_STATE_DISCONNECTED = 0,
    CLOUD_STATE_CONNECTING,
    CLOUD_STATE_CONNECTED,
    CLOUD_STATE_AUTHENTICATED,
    CLOUD_STATE_ERROR
} cloud_state_t;

/** Transport types */
typedef enum {
    CLOUD_TRANSPORT_NONE = 0,
    CLOUD_TRANSPORT_WEBSOCKET,
    CLOUD_TRANSPORT_COAP,
    CLOUD_TRANSPORT_MQTT,
    CLOUD_TRANSPORT_BLE,
    CLOUD_TRANSPORT_HTTP,
} cloud_transport_t;

/** Source connection info */
typedef struct {
    msg_source_t source;
    cloud_transport_t transport;
    cloud_state_t state;
    char url[128];
    bool authenticated;
    uint32_t last_activity;
    uint32_t messages_rx;
    uint32_t messages_tx;
} cloud_source_info_t;

/** Cloud client configuration */
typedef struct {
    /** Remote server URL (WebSocket/CoAP/MQTT) */
    const char *server_url;
    
    /** Device ID for authentication */
    const char *device_id;
    
    /** Authentication token/key */
    const char *auth_token;
    
    /** Auto-connect on init */
    bool auto_connect;
    
    /** Auto-reconnect on disconnect */
    bool auto_reconnect;
    
    /** Reconnect delay (ms) */
    uint32_t reconnect_delay_ms;
    
    /** Heartbeat interval (ms, 0 to disable) */
    uint32_t heartbeat_interval_ms;
    
    /** Enable Bluetooth (AkiraApp) */
    bool enable_bluetooth;
    
    /** Enable local web server messages */
    bool enable_webserver;
} cloud_client_config_t;

/*===========================================================================*/
/* Handler Callbacks                                                         */
/*===========================================================================*/

/**
 * @brief Message handler callback
 * @param msg Received message
 * @param source Which source sent it
 * @return 0 to continue processing, non-zero to stop
 */
typedef int (*cloud_msg_handler_t)(const cloud_message_t *msg, msg_source_t source);

/**
 * @brief Connection state change callback
 * @param source Which source changed
 * @param state New state
 */
typedef void (*cloud_state_handler_t)(msg_source_t source, cloud_state_t state);

/**
 * @brief OTA data handler
 * @param data Firmware chunk data
 * @param len Chunk length
 * @param offset Offset in firmware
 * @param total Total size
 * @param source Which source is sending
 * @return 0 on success
 */
typedef int (*cloud_ota_data_handler_t)(const uint8_t *data, size_t len,
                                         size_t offset, size_t total,
                                         msg_source_t source);

/**
 * @brief App data handler
 * @param app_id App identifier
 * @param data WASM chunk data
 * @param len Chunk length
 * @param offset Offset in file
 * @param total Total size
 * @param source Which source is sending
 * @return 0 on success
 */
typedef int (*cloud_app_data_handler_t)(const char *app_id,
                                         const uint8_t *data, size_t len,
                                         size_t offset, size_t total,
                                         msg_source_t source);

/**
 * @brief App complete handler
 * @param app_id App identifier
 * @param metadata App metadata
 * @param success Transfer succeeded
 * @param source Which source sent it
 */
typedef void (*cloud_app_complete_handler_t)(const char *app_id,
                                              const payload_app_metadata_t *metadata,
                                              bool success,
                                              msg_source_t source);

/*===========================================================================*/
/* Initialization                                                            */
/*===========================================================================*/

/**
 * @brief Initialize cloud client
 * @param config Configuration (NULL for defaults)
 * @return 0 on success
 */
int cloud_client_init(const cloud_client_config_t *config);

/**
 * @brief Deinitialize cloud client
 * @return 0 on success
 */
int cloud_client_deinit(void);

/**
 * @brief Check if initialized
 * @return true if initialized
 */
bool cloud_client_is_initialized(void);

/*===========================================================================*/
/* Connection Management                                                     */
/*===========================================================================*/

/**
 * @brief Connect to remote server
 * @param url Server URL (NULL to use configured)
 * @return 0 on success
 */
int cloud_client_connect(const char *url);

/**
 * @brief Disconnect from remote server
 * @return 0 on success
 */
int cloud_client_disconnect(void);

/**
 * @brief Get connection state for a source
 * @param source Message source
 * @return Connection state
 */
cloud_state_t cloud_client_get_state(msg_source_t source);

/**
 * @brief Get info for all connected sources
 * @param info Output array
 * @param max_count Max entries
 * @return Number of sources
 */
int cloud_client_get_sources(cloud_source_info_t *info, int max_count);

/*===========================================================================*/
/* Handler Registration                                                      */
/*===========================================================================*/

/**
 * @brief Register handler for message category
 * @param category Message category (MSG_CAT_*)
 * @param handler Handler callback
 * @return 0 on success
 */
int cloud_client_register_handler(msg_category_t category, cloud_msg_handler_t handler);

/**
 * @brief Register connection state handler
 * @param handler State change callback
 * @return 0 on success
 */
int cloud_client_register_state_handler(cloud_state_handler_t handler);

/**
 * @brief Register OTA data handler
 * @param handler OTA chunk handler
 * @return 0 on success
 */
int cloud_client_register_ota_handler(cloud_ota_data_handler_t handler);

/**
 * @brief Register App data handler
 * @param data_handler App chunk handler
 * @param complete_handler App complete handler
 * @return 0 on success
 */
int cloud_client_register_app_handler(cloud_app_data_handler_t data_handler,
                                       cloud_app_complete_handler_t complete_handler);

/*===========================================================================*/
/* Sending Messages                                                          */
/*===========================================================================*/

/**
 * @brief Send message to specific source
 * @param msg Message to send
 * @param dest Destination source
 * @return 0 on success
 */
int cloud_client_send(const cloud_message_t *msg, msg_source_t dest);

/**
 * @brief Send message to all connected sources
 * @param msg Message to send
 * @return Number of successful sends
 */
int cloud_client_broadcast(const cloud_message_t *msg);

/**
 * @brief Send raw data (for binary protocols)
 * @param data Data to send
 * @param len Data length
 * @param dest Destination source
 * @return 0 on success
 */
int cloud_client_send_raw(const uint8_t *data, size_t len, msg_source_t dest);

/*===========================================================================*/
/* High-Level Operations                                                     */
/*===========================================================================*/

/**
 * @brief Send device status
 * @param dest Destination (MSG_SOURCE_CLOUD, etc. or 0 for all)
 * @return 0 on success
 */
int cloud_client_send_status(msg_source_t dest);

/**
 * @brief Request firmware update check
 * @return 0 on success
 */
int cloud_client_check_firmware(void);

/**
 * @brief Request firmware download
 * @param version Firmware version string (NULL for latest)
 * @return 0 on success
 */
int cloud_client_request_firmware(const char *version);

/**
 * @brief Request app catalog
 * @return 0 on success
 */
int cloud_client_request_app_list(void);

/**
 * @brief Request app download
 * @param app_id App identifier
 * @return 0 on success
 */
int cloud_client_request_app(const char *app_id);

/**
 * @brief Check for app updates
 * @return 0 on success
 */
int cloud_client_check_app_updates(void);

/**
 * @brief Send sensor data
 * @param data Sensor data buffer
 * @param len Data length
 * @return 0 on success
 */
int cloud_client_send_sensor_data(const uint8_t *data, size_t len);

/**
 * @brief Send heartbeat to keep connections alive
 * @return 0 on success
 */
int cloud_client_heartbeat(void);

/*===========================================================================*/
/* Bluetooth (AkiraApp) Interface                                            */
/*===========================================================================*/

/**
 * @brief Handle incoming Bluetooth data (called by BT stack)
 * @param data Received data
 * @param len Data length
 * @return 0 on success
 */
int cloud_client_bt_receive(const uint8_t *data, size_t len);

/**
 * @brief Notify Bluetooth connection state change
 * @param connected true if connected
 */
void cloud_client_bt_connected(bool connected);

/*===========================================================================*/
/* Web Server Interface                                                      */
/*===========================================================================*/

/**
 * @brief Handle incoming WebSocket message (called by HTTP server)
 * @param data Message data
 * @param len Data length
 * @param is_binary Binary or text message
 * @return 0 on success
 */
int cloud_client_ws_receive(const uint8_t *data, size_t len, bool is_binary);

/*===========================================================================*/
/* Statistics                                                                */
/*===========================================================================*/

/** Cloud client statistics */
typedef struct {
    uint32_t total_messages_rx;
    uint32_t total_messages_tx;
    uint32_t total_bytes_rx;
    uint32_t total_bytes_tx;
    uint32_t ota_chunks_rx;
    uint32_t app_chunks_rx;
    uint32_t errors;
    uint32_t reconnects;
} cloud_client_stats_t;

/**
 * @brief Get client statistics
 * @param stats Output statistics
 */
void cloud_client_get_stats(cloud_client_stats_t *stats);

/**
 * @brief Reset statistics
 */
void cloud_client_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CLOUD_CLIENT_H */
