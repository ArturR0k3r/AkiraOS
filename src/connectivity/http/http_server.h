/**
 * @file http_server.h
 * @brief HTTP Server for AkiraOS
 *
 * Generic HTTP/WebSocket server that can be used for:
 * - Web UI
 * - REST API
 * - OTA uploads (when requested)
 * - Real-time data streaming (WebSocket)
 */

#ifndef AKIRA_HTTP_SERVER_H
#define AKIRA_HTTP_SERVER_H

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

#define HTTP_SERVER_PORT 8080
#define WEBSOCKET_SERVER_PORT 8081
#define HTTP_MAX_CLIENTS 4
#define HTTP_BUFFER_SIZE 2048

    /** HTTP server states */
    typedef enum
    {
        HTTP_SERVER_STOPPED = 0,
        HTTP_SERVER_STARTING,
        HTTP_SERVER_RUNNING,
        HTTP_SERVER_ERROR
    } http_server_state_t;

    /** HTTP methods */
    typedef enum
    {
        HTTP_GET = 0,
        HTTP_POST,
        HTTP_PUT,
        HTTP_DELETE,
        HTTP_OPTIONS,
        HTTP_PATCH
    } http_method_t;

    /** HTTP content types */
    typedef enum
    {
        HTTP_CONTENT_HTML = 0,
        HTTP_CONTENT_JSON,
        HTTP_CONTENT_TEXT,
        HTTP_CONTENT_BINARY,
        HTTP_CONTENT_FORM
    } http_content_type_t;

    /*===========================================================================*/
    /* Request/Response Structures                                               */
    /*===========================================================================*/

    /** HTTP request */
    typedef struct
    {
        http_method_t method;
        const char *path;
        const char *query;
        const char *body;
        size_t body_len;
        http_content_type_t content_type;
        size_t content_length;

        /* Headers access */
        const char *(*get_header)(const char *name);
    } http_request_t;

    /** HTTP response */
    typedef struct
    {
        int status_code;
        http_content_type_t content_type;

        int (*set_header)(const char *name, const char *value);
        int (*send)(const char *data, size_t len);
        int (*send_file)(const char *path);
        int (*send_json)(const char *json);
    } http_response_t;

    /*===========================================================================*/
    /* Handler Types                                                             */
    /*===========================================================================*/

    /** Request handler callback */
    typedef int (*http_handler_t)(const http_request_t *req, http_response_t *res, void *user_data);

    /** WebSocket message callback */
    typedef void (*ws_message_cb_t)(int client_id, const uint8_t *data, size_t len, void *user_data);

    /** WebSocket connect/disconnect callback */
    typedef void (*ws_event_cb_t)(int client_id, bool connected, void *user_data);

    /** Upload chunk callback (for streaming large files) */
    typedef int (*upload_chunk_cb_t)(const uint8_t *data, size_t len, size_t offset,
                                     size_t total, void *user_data);

    /*===========================================================================*/
    /* Route Registration                                                        */
    /*===========================================================================*/

    /** Route definition */
    typedef struct
    {
        http_method_t method;
        const char *path; /**< Path pattern (supports wildcards: /api/*) */
        http_handler_t handler;
        void *user_data;
    } http_route_t;

    /*===========================================================================*/
    /* HTTP Server API                                                           */
    /*===========================================================================*/

    /**
     * @brief Initialize HTTP server
     * @return 0 on success
     */
    int akira_http_server_init(void);

    /**
     * @brief Start HTTP server
     * @return 0 on success
     */
    int akira_http_server_start(void);

    /**
     * @brief Stop HTTP server
     * @return 0 on success
     */
    int akira_http_server_stop(void);

    /**
     * @brief Get server state
     * @return Current state
     */
    http_server_state_t akira_http_server_get_state(void);

    /**
     * @brief Check if server is running
     * @return true if running
     */
    bool akira_http_server_is_running(void);

    /**
     * @brief Register a route handler
     * @param route Route definition
     * @return 0 on success
     */
    int akira_http_register_route(const http_route_t *route);

    /**
     * @brief Unregister a route
     * @param method HTTP method
     * @param path Path pattern
     * @return 0 on success
     */
    int akira_http_unregister_route(http_method_t method, const char *path);

    /**
     * @brief Register upload handler for streaming file uploads
     * @param path Upload endpoint path
     * @param callback Chunk callback
     * @param user_data User data
     * @return 0 on success
     */
    int akira_http_register_upload_handler(const char *path, upload_chunk_cb_t callback,
                                           void *user_data);

    /**
     * @brief Set static file directory
     * @param path Path to static files
     * @return 0 on success
     */
    int akira_http_set_static_dir(const char *path);

    /**
     * @brief Notify network status change
     * @param connected true if network connected
     * @param ip_address IP address string
     */
    void akira_http_notify_network(bool connected, const char *ip_address);

    /*===========================================================================*/
    /* WebSocket API                                                             */
    /*===========================================================================*/

    /**
     * @brief Enable WebSocket support
     * @return 0 on success
     */
    int akira_http_enable_websocket(void);

    /**
     * @brief Register WebSocket message callback
     * @param callback Message callback
     * @param user_data User data
     * @return 0 on success
     */
    int akira_http_ws_register_message_cb(ws_message_cb_t callback, void *user_data);

    /**
     * @brief Register WebSocket event callback
     * @param callback Event callback
     * @param user_data User data
     * @return 0 on success
     */
    int akira_http_ws_register_event_cb(ws_event_cb_t callback, void *user_data);

    /**
     * @brief Send message to WebSocket client
     * @param client_id Client ID (-1 for broadcast)
     * @param data Message data
     * @param len Data length
     * @return 0 on success
     */
    int akira_http_ws_send(int client_id, const uint8_t *data, size_t len);

    /**
     * @brief Send text message to WebSocket client
     * @param client_id Client ID (-1 for broadcast)
     * @param text Text message
     * @return 0 on success
     */
    int akira_http_ws_send_text(int client_id, const char *text);

    /**
     * @brief Disconnect WebSocket client
     * @param client_id Client ID
     * @return 0 on success
     */
    int akira_http_ws_disconnect(int client_id);

    /**
     * @brief Get number of connected WebSocket clients
     * @return Client count
     */
    int akira_http_ws_client_count(void);

    /*===========================================================================*/
    /* Statistics                                                                */
    /*===========================================================================*/

    /** HTTP server statistics */
    typedef struct
    {
        http_server_state_t state;
        uint32_t requests_handled;
        uint32_t active_connections;
        uint64_t bytes_sent;
        uint64_t bytes_received;
        int ws_clients;
        char server_ip[16];
    } http_server_stats_t;

    /**
     * @brief Get server statistics
     * @param stats Output buffer
     * @return 0 on success
     */
    int akira_http_get_stats(http_server_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_HTTP_SERVER_H */
