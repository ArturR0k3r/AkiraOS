/**
 * @file web_server.h
 * @brief Web Server and OTA Handler Module
 *
 * Self-contained web server module that runs on its own thread,
 * handles HTTP requests, OTA uploads, and provides web interface
 * for the ESP32 gaming device.
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

/* Web server configuration */
#define WEB_SERVER_STACK_SIZE 4096
#define WEB_SERVER_THREAD_PRIORITY 7
#define HTTP_PORT 8080
#define WEBSOCKET_PORT 8081
#define MAX_CONCURRENT_CLIENTS 5
#define UPLOAD_CHUNK_SIZE 1024

/* Web server states */
enum web_server_state
{
    WEB_SERVER_STOPPED = 0,
    WEB_SERVER_STARTING,
    WEB_SERVER_RUNNING,
    WEB_SERVER_ERROR
};

/* Web server statistics */
struct web_server_stats
{
    uint32_t requests_handled;
    uint32_t ota_uploads;
    uint32_t active_connections;
    uint64_t bytes_transferred;
    enum web_server_state state;
};

/* Callback function types */
typedef int (*system_info_cb_t)(char *buffer, size_t buffer_size);
typedef int (*button_state_cb_t)(char *buffer, size_t buffer_size);
typedef int (*settings_info_cb_t)(char *buffer, size_t buffer_size);
typedef int (*shell_command_cb_t)(const char *command, char *response, size_t response_size);

/* Web server callbacks structure */
struct web_server_callbacks
{
    system_info_cb_t get_system_info;
    button_state_cb_t get_button_state;
    settings_info_cb_t get_settings_info;
    shell_command_cb_t execute_shell_command;
};

/**
 * @brief Initialize and start the web server
 *
 * Creates the web server thread and starts HTTP server.
 * The server will wait for network connectivity before becoming active.
 *
 * @param callbacks Callback functions for data retrieval
 * @return 0 on success, negative error code on failure
 */
int web_server_start(const struct web_server_callbacks *callbacks);

/**
 * @brief Stop the web server
 *
 * Gracefully shuts down the HTTP server and terminates the thread.
 *
 * @return 0 on success, negative error code on failure
 */
int web_server_stop(void);

/**
 * @brief Get web server statistics
 *
 * @param stats Output buffer for statistics
 * @return 0 on success, negative error code on failure
 */
int web_server_get_stats(struct web_server_stats *stats);

/**
 * @brief Check if web server is running
 *
 * @return true if server is running and accepting connections
 */
bool web_server_is_running(void);

/**
 * @brief Get current web server state
 *
 * @return Current web server state
 */
enum web_server_state web_server_get_state(void);

/**
 * @brief Notify web server of network status change
 *
 * Call this when WiFi connection status changes to update server behavior.
 *
 * @param connected true if network is connected, false otherwise
 * @param ip_address IP address string (can be NULL if not connected)
 */
void web_server_notify_network_status(bool connected, const char *ip_address);

/**
 * @brief Send log message to connected WebSocket clients
 *
 * @param message Log message to broadcast
 * @param length Message length
 */
void web_server_broadcast_log(const char *message, size_t length);

/**
 * @brief Force refresh of cached data
 *
 * Triggers callbacks to refresh system information, button states, etc.
 */
void web_server_refresh_data(void);

/**
 * @brief Set custom HTTP response headers
 *
 * @param headers Additional headers to include in HTTP responses
 */
void web_server_set_custom_headers(const char *headers);

#endif /* WEB_SERVER_H */