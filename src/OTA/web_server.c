/**
 * @file web_server.c
 * @brief Optimized Web Server Implementation for ESP32
 *
 */

#include "web_server.h"
#include "ota_manager.h"

/* WebServer OTA transport implementation */
static int webserver_ota_start(void *user_data)
{
    /* Start OTA via HTTP upload */
    return OTA_OK;
}

static int webserver_ota_stop(void *user_data)
{
    /* Stop OTA upload */
    return OTA_OK;
}

static int webserver_ota_send_chunk(const uint8_t *data, size_t len, void *user_data)
{
    /* Pass chunk to OTA manager */
    // Example: ota_manager_write_chunk(data, len);
    return OTA_OK;
}

static int webserver_ota_report_progress(uint8_t percent, void *user_data)
{
    /* Report progress to HTTP client */
    return OTA_OK;
}

static ota_transport_t webserver_ota_transport = {
    .name = "webserver",
    .start = webserver_ota_start,
    .stop = webserver_ota_stop,
    .send_chunk = webserver_ota_send_chunk,
    .report_progress = webserver_ota_report_progress,
    .user_data = NULL};

static void register_webserver_ota_transport(void)
{
    ota_manager_register_transport(&webserver_ota_transport);
}

/* Call register_webserver_ota_transport() during web server init */
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
#include "akira/akira.h"

LOG_MODULE_REGISTER(web_server, AKIRA_LOG_LEVEL);

/* Optimized constants */
#define HTTP_BUFFER_SIZE 1024
#define HTTP_RESPONSE_BUFFER_SIZE 2048
#define UPLOAD_CHUNK_SIZE 512
#define MAX_CONNECTIONS 2

/* Thread stack - optimized size */
static K_THREAD_STACK_DEFINE(web_server_stack, 4096);
static struct k_thread web_server_thread_data;
static k_tid_t web_server_thread_id;

/* Compact server state */
static struct
{
    enum web_server_state state;
    uint32_t requests_handled;
    uint32_t bytes_transferred;
    uint8_t active_connections;
    bool network_connected;
    char server_ip[16];
} server_state __aligned(4);

static struct web_server_callbacks callbacks = {0};
static K_MUTEX_DEFINE(server_mutex);

/* Message queue - reduced size */
#define SERVER_MSG_QUEUE_SIZE 8

enum server_msg_type
{
    MSG_START_SERVER = 1,
    MSG_STOP_SERVER,
    MSG_NETWORK_STATUS_CHANGED
};

struct server_msg
{
    enum server_msg_type type : 8;
    uint8_t reserved;
    union
    {
        struct
        {
            bool connected;
            char ip_address[16];
        } network_status;
    } data;
} __packed;

K_MSGQ_DEFINE(server_msgq, sizeof(struct server_msg), SERVER_MSG_QUEUE_SIZE, 4);

/* Compressed HTML - critical parts only */
static const char html_header[] =
    "<!DOCTYPE html><html><head><title>ESP32 OTA</title>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:sans-serif;margin:20px;background:#f0f0f0}"
    ".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:8px}"
    ".panel{background:#f8f9fa;padding:15px;margin:10px 0;border-radius:5px;border-left:4px solid #007bff}"
    ".btn{background:#007bff;color:white;padding:8px 16px;border:none;border-radius:4px;cursor:pointer;margin:5px}"
    ".btn:hover{background:#0056b3}"
    ".btn-success{background:#28a745}"
    ".btn-danger{background:#dc3545}"
    ".progress{width:100%;height:20px;background:#e9ecef;border-radius:4px;overflow:hidden}"
    ".progress-bar{height:100%;background:#007bff;transition:width 0.3s}"
    ".status{padding:5px 10px;border-radius:3px;font-size:12px;margin:5px 0}"
    ".online{background:#d4edda;color:#155724}"
    ".offline{background:#f8d7da;color:#721c24}"
    "</style></head><body><div class='container'>";

static const char html_footer[] = "</div></body></html>";

static size_t parse_content_length(const char *request_data)
{
    const char *content_length_header = strstr(request_data, "Content-Length:");
    if (!content_length_header)
    {
        return 0; // No Content-Length header
    }

    content_length_header += 15; // Skip "Content-Length:"

    /* Skip whitespace */
    while (*content_length_header == ' ' || *content_length_header == '\t')
    {
        content_length_header++;
    }

    /* Parse number with bounds checking */
    char *endptr;
    unsigned long length = strtoul(content_length_header, &endptr, 10);

    /* Validate parsing */
    if (endptr == content_length_header || length > SIZE_MAX)
    {
        LOG_ERR("Invalid Content-Length value");
        return 0;
    }

    /* Reasonable size limits for embedded device */
    if (length > (2 * 1024 * 1024))
    { // Max 2MB
        LOG_ERR("Content-Length too large: %lu", length);
        return 0;
    }

    return (size_t)length;
}

static int find_multipart_boundary(const char *request_data, char *boundary, size_t boundary_size)
{
    const char *content_type = strstr(request_data, "Content-Type:");
    if (!content_type)
    {
        return -1;
    }

    const char *boundary_start = strstr(content_type, "boundary=");
    if (!boundary_start)
    {
        return -1;
    }

    boundary_start += 9; // Skip "boundary="

    /* Find end of boundary (space, newline, or semicolon) */
    const char *boundary_end = boundary_start;
    while (*boundary_end && *boundary_end != ' ' &&
           *boundary_end != '\r' && *boundary_end != '\n' &&
           *boundary_end != ';')
    {
        boundary_end++;
    }

    size_t boundary_len = boundary_end - boundary_start;
    if (boundary_len == 0 || boundary_len >= boundary_size - 2)
    {
        return -1;
    }

    /* Add "--" prefix for multipart boundary */
    boundary[0] = '-';
    boundary[1] = '-';
    memcpy(boundary + 2, boundary_start, boundary_len);
    boundary[boundary_len + 2] = '\0';

    return 0;
}

/* HTTP response helpers */
static int send_http_response(int client_fd, int status_code, const char *content_type,
                              const char *body, size_t body_len)
{
    char response[HTTP_RESPONSE_BUFFER_SIZE];
    int header_len;

    if (body_len == 0 && body)
    {
        body_len = strlen(body);
    }

    header_len = snprintf(response, sizeof(response),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "Cache-Control: no-cache\r\n"
                          "\r\n",
                          status_code,
                          (status_code == 200) ? "OK" : "Error",
                          content_type,
                          body_len);

    if (header_len >= sizeof(response))
    {
        return -1;
    }

    /* Send header */
    if (send(client_fd, response, header_len, 0) != header_len)
    {
        return -1;
    }

    /* Send body if present */
    if (body && body_len > 0)
    {
        if (send(client_fd, body, body_len, 0) != body_len)
        {
            return -1;
        }
    }

    return 0;
}

/* Optimized HTML generation */
static int generate_main_page(char *buffer, size_t buffer_size)
{
    const struct ota_progress *ota = ota_get_progress();
    size_t used = 0;

    /* Header */
    used += snprintf(buffer + used, buffer_size - used, "%s", html_header);

    /* System status panel */
    used += snprintf(buffer + used, buffer_size - used,
                     "<div class='panel'><h3>System Status</h3>"
                     "<div>Device: <span class='status online'>Online</span></div>"
                     "<div>IP: %s</div>"
                     "<div>OTA State: %s (%d%%)</div>"
                     "</div>",
                     server_state.server_ip,
                     ota_state_to_string(ota->state),
                     ota->percentage);

    /* OTA panel */
    used += snprintf(buffer + used, buffer_size - used,
                     "<div class='panel'><h3>Firmware Update</h3>"
                     "<form action='/upload' method='post' enctype='multipart/form-data'>"
                     "<input type='file' name='firmware' accept='.bin' required>"
                     "<button type='submit' class='btn'>Upload</button>"
                     "</form>");

    if (ota->state == OTA_STATE_RECEIVING || ota->state == OTA_STATE_VALIDATING)
    {
        used += snprintf(buffer + used, buffer_size - used,
                         "<div class='progress'><div class='progress-bar' style='width:%d%%'></div></div>",
                         ota->percentage);
    }

    used += snprintf(buffer + used, buffer_size - used, "</div>");

    /* Control panel */
    used += snprintf(buffer + used, buffer_size - used,
                     "<div class='panel'><h3>System Control</h3>"
                     "<button class='btn btn-success' onclick='confirmFirmware()'>Confirm Firmware</button>"
                     "<button class='btn btn-danger' onclick='rebootSystem()'>Reboot</button>"
                     "</div>");

    /* JavaScript */
    used += snprintf(buffer + used, buffer_size - used,
                     "<script>"
                     "function confirmFirmware(){if(confirm('Confirm firmware?')){"
                     "fetch('/api/ota/confirm',{method:'POST'}).then(r=>r.text())"
                     ".then(d=>alert(d)).catch(e=>alert('Failed'))}}"
                     "function rebootSystem(){if(confirm('Reboot device?')){"
                     "fetch('/api/reboot',{method:'POST'}).then(()=>{"
                     "alert('Rebooting...');setTimeout(()=>location.reload(),5000)"
                     "}).catch(e=>alert('Failed'))}}"
                     "setTimeout(()=>location.reload(),30000);" // Auto-refresh
                     "</script>");

    /* Footer */
    used += snprintf(buffer + used, buffer_size - used, "%s", html_footer);

    return (used < buffer_size) ? 0 : -1;
}

/* HTTP request parser */
static int parse_http_request(const char *buffer, char *method, char *path, size_t path_size)
{
    const char *space1 = strchr(buffer, ' ');
    const char *space2 = space1 ? strchr(space1 + 1, ' ') : NULL;

    if (!space1 || !space2)
    {
        return -1;
    }

    /* Extract method */
    size_t method_len = space1 - buffer;
    if (method_len >= 8)
        return -1; // Max method length

    memcpy(method, buffer, method_len);
    method[method_len] = '\0';

    /* Extract path */
    size_t path_len = space2 - space1 - 1;
    if (path_len >= path_size)
        return -1;

    memcpy(path, space1 + 1, path_len);
    path[path_len] = '\0';

    return 0;
}

/* Handle firmware upload */
static int handle_firmware_upload(int client_fd, const char *request_headers, size_t content_length)
{
    if (content_length == 0 || content_length > (2 * 1024 * 1024))
    {
        send_http_response(client_fd, 400, "text/plain", "Invalid file size", 0);
        return -1;
    }

    /* Find multipart boundary */
    char boundary[128];
    if (find_multipart_boundary(request_headers, boundary, sizeof(boundary)) != 0)
    {
        send_http_response(client_fd, 400, "text/plain", "Invalid multipart format", 0);
        return -1;
    }

    LOG_INF("Using multipart boundary: %s", boundary);

    /* Start OTA update */
    enum ota_result result = ota_start_update(content_length);
    if (result != OTA_OK)
    {
        send_http_response(client_fd, 500, "text/plain", ota_result_to_string(result), 0);
        return -1;
    }

    /* Receive multipart data */
    char upload_buffer[UPLOAD_CHUNK_SIZE];
    size_t total_received = 0;
    bool found_file_data = false;
    size_t boundary_len = strlen(boundary);

    while (total_received < content_length)
    {
        size_t chunk_size = MIN(UPLOAD_CHUNK_SIZE, content_length - total_received);
        ssize_t received = recv(client_fd, upload_buffer, chunk_size, 0);

        if (received <= 0)
        {
            LOG_ERR("Receive failed during upload: %d", errno);
            ota_abort_update();
            return -1;
        }

        /* Look for file data start if not found yet */
        if (!found_file_data)
        {
            /* Look for double CRLF indicating end of headers */
            char *data_start = strstr(upload_buffer, "\r\n\r\n");
            if (data_start)
            {
                data_start += 4; // Skip \r\n\r\n
                found_file_data = true;

                /* Write only the file data part */
                size_t file_data_len = received - (data_start - upload_buffer);
                if (file_data_len > 0)
                {
                    result = ota_write_chunk((uint8_t *)data_start, file_data_len);
                    if (result != OTA_OK)
                    {
                        ota_abort_update();
                        send_http_response(client_fd, 500, "text/plain",
                                           ota_result_to_string(result), 0);
                        return -1;
                    }
                }
            }
        }
        else
        {
            /* Check for boundary indicating end of file data */
            if (memmem(upload_buffer, received, boundary, boundary_len))
            {
                /* Found boundary - stop processing file data */
                break;
            }

            /* Write chunk to OTA */
            result = ota_write_chunk((uint8_t *)upload_buffer, received);
            if (result != OTA_OK)
            {
                ota_abort_update();
                send_http_response(client_fd, 500, "text/plain",
                                   ota_result_to_string(result), 0);
                return -1;
            }
        }

        total_received += received;
    }

    if (!found_file_data)
    {
        ota_abort_update();
        send_http_response(client_fd, 400, "text/plain", "No file data found", 0);
        return -1;
    }

    /* Finalize update */
    result = ota_finalize_update();
    if (result != OTA_OK)
    {
        send_http_response(client_fd, 500, "text/plain", ota_result_to_string(result), 0);
        return -1;
    }

    /* Send redirect to main page */
    const char *redirect_response =
        "HTTP/1.1 302 Found\r\n"
        "Location: /\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";

    send(client_fd, redirect_response, strlen(redirect_response), 0);

    /* Schedule reboot */
    ota_reboot_to_apply_update(3000);
    return 0;
}

/* Handle API requests */
static int handle_api_request(int client_fd, const char *path)
{
    char response[512];

    if (strcmp(path, "/api/ota/status") == 0)
    {
        const struct ota_progress *ota = ota_get_progress();
        snprintf(response, sizeof(response),
                 "{\"state\":\"%s\",\"progress\":%d,\"message\":\"%s\"}",
                 ota_state_to_string(ota->state), ota->percentage, ota->status_message);
        return send_http_response(client_fd, 200, "application/json", response, 0);
    }

    if (strcmp(path, "/api/ota/confirm") == 0)
    {
        enum ota_result result = ota_confirm_firmware();
        const char *msg = (result == OTA_OK) ? "Firmware confirmed" : ota_result_to_string(result);
        return send_http_response(client_fd, (result == OTA_OK) ? 200 : 500, "text/plain", msg, 0);
    }

    if (strcmp(path, "/api/reboot") == 0)
    {
        send_http_response(client_fd, 200, "text/plain", "Rebooting", 0);
        ota_reboot_to_apply_update(2000);
        return 0;
    }

    if (strcmp(path, "/api/system") == 0)
    {
        snprintf(response, sizeof(response),
                 "{\"uptime\":\"%.1f hours\",\"memory\":\"Available\",\"wifi\":\"Connected\",\"cpu\":\"ESP32\"}",
                 (float)k_uptime_get() / 3600000.0f);
        return send_http_response(client_fd, 200, "application/json", response, 0);
    }

    return send_http_response(client_fd, 404, "text/plain", "API not found", 0);
}

/* Main HTTP request handler */
static int handle_http_request(int client_fd)
{
    char buffer[HTTP_BUFFER_SIZE];
    char method[16], path[128];
    ssize_t received;

    /* Set receive timeout */
    struct timeval timeout = {.tv_sec = 30, .tv_usec = 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    /* Receive request with timeout */
    received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0)
    {
        LOG_WRN("Request receive failed or timeout: %d", errno);
        return -1;
    }

    buffer[received] = '\0';

    /* Parse request line */
    if (parse_http_request(buffer, method, path, sizeof(path)) != 0)
    {
        send_http_response(client_fd, 400, "text/plain", "Bad Request", 0);
        return -1;
    }

    LOG_DBG("HTTP %s %s", method, path);

    /* Route request */
    if (strcmp(method, "GET") == 0)
    {
        if (strcmp(path, "/") == 0)
        {
            /* Main page */
            static char page_buffer[4096];
            if (generate_main_page(page_buffer, sizeof(page_buffer)) == 0)
            {
                return send_http_response(client_fd, 200, "text/html", page_buffer, 0);
            }
            return send_http_response(client_fd, 500, "text/plain", "Page generation failed", 0);
        }

        if (strncmp(path, "/api/", 5) == 0)
        {
            return handle_api_request(client_fd, path);
        }

        return send_http_response(client_fd, 404, "text/plain", "Not Found", 0);
    }

    if (strcmp(method, "POST") == 0)
    {
        if (strcmp(path, "/upload") == 0)
        {
            /* Parse Content-Length with validation */
            size_t content_length = parse_content_length(buffer);
            if (content_length == 0)
            {
                return send_http_response(client_fd, 400, "text/plain",
                                          "Missing or invalid Content-Length", 0);
            }

            return handle_firmware_upload(client_fd, buffer, content_length);
        }

        if (strncmp(path, "/api/", 5) == 0)
        {
            return handle_api_request(client_fd, path);
        }
    }

    return send_http_response(client_fd, 405, "text/plain", "Method Not Allowed", 0);
}

/* Simplified web server */
static int run_web_server(void)
{
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd < 0)
    {
        LOG_ERR("Socket creation failed: %d", errno);
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(HTTP_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        LOG_ERR("Bind failed: %d", errno);
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, MAX_CONNECTIONS) < 0)
    {
        LOG_ERR("Listen failed: %d", errno);
        close(server_fd);
        return -1;
    }

    LOG_INF("HTTP server listening on port %d", HTTP_PORT);

    while (server_state.state == WEB_SERVER_RUNNING)
    {
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0)
        {
            if (server_state.state == WEB_SERVER_RUNNING)
            {
                LOG_ERR("Accept failed: %d", errno);
            }
            continue;
        }

        /* Handle request */
        if (handle_http_request(client_fd) == 0)
        {
            k_mutex_lock(&server_mutex, K_FOREVER);
            server_state.requests_handled++;
            k_mutex_unlock(&server_mutex);
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}

/* Server operations */
static void do_start_server(void)
{
    if (server_state.state == WEB_SERVER_RUNNING)
    {
        return;
    }

    server_state.state = WEB_SERVER_STARTING;

    if (run_web_server() == 0)
    {
        server_state.state = WEB_SERVER_RUNNING;
        LOG_INF("Web server started");
    }
    else
    {
        server_state.state = WEB_SERVER_ERROR;
        LOG_ERR("Web server start failed");
    }
}

static void do_stop_server(void)
{
    server_state.state = WEB_SERVER_STOPPED;
    LOG_INF("Web server stopped");
}

static void do_network_status_changed(bool connected, const char *ip_address)
{
    server_state.network_connected = connected;

    if (connected && ip_address)
    {
        strncpy(server_state.server_ip, ip_address, sizeof(server_state.server_ip) - 1);
        server_state.server_ip[sizeof(server_state.server_ip) - 1] = '\0';
        LOG_INF("Network connected: http://%s:%d", server_state.server_ip, HTTP_PORT);

        if (server_state.state == WEB_SERVER_STOPPED)
        {
            do_start_server();
        }
    }
    else
    {
        server_state.server_ip[0] = '\0';
        LOG_INF("Network disconnected");
    }
}

/* Web server thread */
static void web_server_thread_main(void *p1, void *p2, void *p3)
{
    struct server_msg msg;

    LOG_INF("Web server thread started");

    while (1)
    {
        if (k_msgq_get(&server_msgq, &msg, K_MSEC(5000)) == 0)
        {
            switch (msg.type)
            {
            case MSG_START_SERVER:
                do_start_server();
                break;
            case MSG_STOP_SERVER:
                do_stop_server();
                break;
            case MSG_NETWORK_STATUS_CHANGED:
                do_network_status_changed(msg.data.network_status.connected,
                                          msg.data.network_status.ip_address);
                break;
            }
        }

        /* Periodic housekeeping */
        if (server_state.state == WEB_SERVER_RUNNING)
        {
            k_mutex_lock(&server_mutex, K_FOREVER);
            server_state.active_connections = 0; // Reset counter
            k_mutex_unlock(&server_mutex);
        }
    }
}

/* Public API */
int web_server_start(const struct web_server_callbacks *cb)
{
    if (cb)
    {
        memcpy(&callbacks, cb, sizeof(callbacks));
    }

    memset(&server_state, 0, sizeof(server_state));
    server_state.state = WEB_SERVER_STOPPED;
    strcpy(server_state.server_ip, "0.0.0.0");

    web_server_thread_id = k_thread_create(&web_server_thread_data,
                                           web_server_stack,
                                           K_THREAD_STACK_SIZEOF(web_server_stack),
                                           web_server_thread_main,
                                           NULL, NULL, NULL,
                                           WEB_SERVER_THREAD_PRIORITY,
                                           0, K_NO_WAIT);

    k_thread_name_set(web_server_thread_id, "web_server");

    LOG_INF("Web server initialized");
    return 0;
}

int web_server_stop(void)
{
    struct server_msg msg = {.type = MSG_STOP_SERVER};
    return (k_msgq_put(&server_msgq, &msg, K_NO_WAIT) == 0) ? 0 : -ENOMEM;
}

int web_server_get_stats(struct web_server_stats *stats)
{
    if (!stats)
        return -EINVAL;

    k_mutex_lock(&server_mutex, K_FOREVER);
    stats->state = server_state.state;
    stats->requests_handled = server_state.requests_handled;
    stats->bytes_transferred = server_state.bytes_transferred;
    stats->active_connections = server_state.active_connections;
    k_mutex_unlock(&server_mutex);

    return 0;
}

bool web_server_is_running(void)
{
    return (server_state.state == WEB_SERVER_RUNNING);
}

enum web_server_state web_server_get_state(void)
{
    return server_state.state;
}

void web_server_notify_network_status(bool connected, const char *ip_address)
{
    struct server_msg msg = {
        .type = MSG_NETWORK_STATUS_CHANGED,
        .data.network_status.connected = connected};

    if (connected && ip_address)
    {
        strncpy(msg.data.network_status.ip_address, ip_address,
                sizeof(msg.data.network_status.ip_address) - 1);
        msg.data.network_status.ip_address[sizeof(msg.data.network_status.ip_address) - 1] = '\0';
    }
    else
    {
        msg.data.network_status.ip_address[0] = '\0';
    }

    k_msgq_put(&server_msgq, &msg, K_NO_WAIT);
}