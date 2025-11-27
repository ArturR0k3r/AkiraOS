/**
 * @file ws_client.h
 * @brief WebSocket Client for AkiraOS
 *
 * Provides WebSocket client functionality for connecting to remote servers
 * for real-time bidirectional communication (cloud sync, remote control, etc.)
 */

#ifndef AKIRA_WS_CLIENT_H
#define AKIRA_WS_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define WS_CLIENT_MAX_URL_LEN       256
#define WS_CLIENT_MAX_CONNECTIONS   4
#define WS_CLIENT_RX_BUFFER_SIZE    2048
#define WS_CLIENT_TX_BUFFER_SIZE    2048

/*===========================================================================*/
/* Types                                                                     */
/*===========================================================================*/

/** WebSocket client handle */
typedef int ws_client_handle_t;

/** WebSocket client states */
typedef enum {
    WS_CLIENT_DISCONNECTED = 0,
    WS_CLIENT_CONNECTING,
    WS_CLIENT_CONNECTED,
    WS_CLIENT_CLOSING,
    WS_CLIENT_ERROR
} ws_client_state_t;

/** WebSocket message types */
typedef enum {
    WS_MSG_TEXT = 0x01,
    WS_MSG_BINARY = 0x02,
    WS_MSG_PING = 0x09,
    WS_MSG_PONG = 0x0A,
    WS_MSG_CLOSE = 0x08
} ws_msg_type_t;

/** WebSocket client events */
typedef enum {
    WS_CLIENT_EVENT_CONNECTED,
    WS_CLIENT_EVENT_DISCONNECTED,
    WS_CLIENT_EVENT_MESSAGE,
    WS_CLIENT_EVENT_ERROR,
    WS_CLIENT_EVENT_PING,
    WS_CLIENT_EVENT_PONG
} ws_client_event_t;

/** WebSocket client configuration */
typedef struct {
    const char *url;              /**< WebSocket URL (ws:// or wss://) */
    const char *subprotocol;      /**< Optional subprotocol */
    uint32_t ping_interval_ms;    /**< Auto-ping interval (0 to disable) */
    uint32_t connect_timeout_ms;  /**< Connection timeout */
    uint32_t reconnect_delay_ms;  /**< Delay before auto-reconnect (0 to disable) */
    bool auto_reconnect;          /**< Enable auto-reconnect on disconnect */
} ws_client_config_t;

/** WebSocket message received callback */
typedef void (*ws_client_message_cb_t)(ws_client_handle_t handle,
                                        ws_msg_type_t type,
                                        const uint8_t *data,
                                        size_t len,
                                        void *user_data);

/** WebSocket event callback */
typedef void (*ws_client_event_cb_t)(ws_client_handle_t handle,
                                      ws_client_event_t event,
                                      void *event_data,
                                      void *user_data);

/*===========================================================================*/
/* Initialization                                                            */
/*===========================================================================*/

/**
 * @brief Initialize WebSocket client subsystem
 * @return 0 on success
 */
int ws_client_init(void);

/**
 * @brief Deinitialize WebSocket client subsystem
 * @return 0 on success
 */
int ws_client_deinit(void);

/*===========================================================================*/
/* Connection Management                                                     */
/*===========================================================================*/

/**
 * @brief Create a new WebSocket client connection
 * @param config Connection configuration
 * @return Client handle (>= 0) or negative error
 */
ws_client_handle_t ws_client_connect(const ws_client_config_t *config);

/**
 * @brief Disconnect WebSocket client
 * @param handle Client handle
 * @param code Close code (1000 = normal)
 * @param reason Close reason string (optional)
 * @return 0 on success
 */
int ws_client_disconnect(ws_client_handle_t handle, uint16_t code, const char *reason);

/**
 * @brief Get client connection state
 * @param handle Client handle
 * @return Current state
 */
ws_client_state_t ws_client_get_state(ws_client_handle_t handle);

/**
 * @brief Check if client is connected
 * @param handle Client handle
 * @return true if connected
 */
bool ws_client_is_connected(ws_client_handle_t handle);

/*===========================================================================*/
/* Sending Data                                                              */
/*===========================================================================*/

/**
 * @brief Send text message
 * @param handle Client handle
 * @param text Text to send
 * @return 0 on success
 */
int ws_client_send_text(ws_client_handle_t handle, const char *text);

/**
 * @brief Send binary data
 * @param handle Client handle
 * @param data Data to send
 * @param len Data length
 * @return 0 on success
 */
int ws_client_send_binary(ws_client_handle_t handle, const uint8_t *data, size_t len);

/**
 * @brief Send ping frame
 * @param handle Client handle
 * @return 0 on success
 */
int ws_client_send_ping(ws_client_handle_t handle);

/*===========================================================================*/
/* Callbacks                                                                 */
/*===========================================================================*/

/**
 * @brief Register message callback
 * @param handle Client handle
 * @param callback Message callback
 * @param user_data User data passed to callback
 * @return 0 on success
 */
int ws_client_set_message_cb(ws_client_handle_t handle,
                              ws_client_message_cb_t callback,
                              void *user_data);

/**
 * @brief Register event callback
 * @param handle Client handle
 * @param callback Event callback
 * @param user_data User data passed to callback
 * @return 0 on success
 */
int ws_client_set_event_cb(ws_client_handle_t handle,
                            ws_client_event_cb_t callback,
                            void *user_data);

/*===========================================================================*/
/* Utility                                                                   */
/*===========================================================================*/

/**
 * @brief Set custom HTTP headers for handshake
 * @param handle Client handle
 * @param name Header name
 * @param value Header value
 * @return 0 on success
 */
int ws_client_set_header(ws_client_handle_t handle, const char *name, const char *value);

/**
 * @brief Get server URL for connection
 * @param handle Client handle
 * @return URL string or NULL
 */
const char *ws_client_get_url(ws_client_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_WS_CLIENT_H */
