/**
 * @file http_server.c
 * @brief HTTP Server Implementation for AkiraOS
 */

#include "http_server.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(http_server, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define MAX_ROUTES 16
#define MAX_WS_CLIENTS 4
#define SERVER_THREAD_STACK_SIZE 4096
#define SERVER_THREAD_PRIORITY 7

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct {
    bool initialized;
    http_server_state_t state;
    http_server_stats_t stats;
    
    /* Routes */
    http_route_t routes[MAX_ROUTES];
    int route_count;
    
    /* Upload handler */
    const char *upload_path;
    upload_chunk_cb_t upload_cb;
    void *upload_user_data;
    
    /* WebSocket */
    bool ws_enabled;
    ws_message_cb_t ws_msg_cb;
    void *ws_msg_user_data;
    ws_event_cb_t ws_event_cb;
    void *ws_event_user_data;
    int ws_client_fds[MAX_WS_CLIENTS];
    
    /* Server socket */
    int server_fd;
    bool running;
    
    struct k_mutex mutex;
} http_srv;

/* Thread */
static K_THREAD_STACK_DEFINE(server_stack, SERVER_THREAD_STACK_SIZE);
static struct k_thread server_thread;

/*===========================================================================*/
/* HTTP Parsing Helpers                                                      */
/*===========================================================================*/

static const char *http_status_text(int code)
{
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

static const char *content_type_str(http_content_type_t type)
{
    switch (type) {
        case HTTP_CONTENT_HTML: return "text/html; charset=utf-8";
        case HTTP_CONTENT_JSON: return "application/json";
        case HTTP_CONTENT_TEXT: return "text/plain";
        case HTTP_CONTENT_BINARY: return "application/octet-stream";
        case HTTP_CONTENT_FORM: return "application/x-www-form-urlencoded";
        default: return "text/plain";
    }
}

static http_method_t parse_method(const char *method)
{
    if (strcmp(method, "GET") == 0) return HTTP_GET;
    if (strcmp(method, "POST") == 0) return HTTP_POST;
    if (strcmp(method, "PUT") == 0) return HTTP_PUT;
    if (strcmp(method, "DELETE") == 0) return HTTP_DELETE;
    if (strcmp(method, "OPTIONS") == 0) return HTTP_OPTIONS;
    if (strcmp(method, "PATCH") == 0) return HTTP_PATCH;
    return HTTP_GET;
}

static bool path_matches(const char *pattern, const char *path)
{
    /* Simple wildcard matching */
    while (*pattern && *path) {
        if (*pattern == '*') {
            return true;  /* Wildcard matches rest */
        }
        if (*pattern != *path) {
            return false;
        }
        pattern++;
        path++;
    }
    
    return (*pattern == *path) || (*pattern == '*');
}

/*===========================================================================*/
/* Response Implementation                                                   */
/*===========================================================================*/

typedef struct {
    int client_fd;
    bool headers_sent;
    int status_code;
    http_content_type_t content_type;
    char headers[512];
    size_t headers_len;
} response_ctx_t;

static int resp_set_header(const char *name, const char *value)
{
    /* TODO: Implement header setting */
    return 0;
}

static int resp_send(response_ctx_t *ctx, const char *data, size_t len)
{
    if (!ctx->headers_sent) {
        char header[256];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            ctx->status_code, http_status_text(ctx->status_code),
            content_type_str(ctx->content_type), len);
        
        send(ctx->client_fd, header, hlen, 0);
        ctx->headers_sent = true;
        http_srv.stats.bytes_sent += hlen;
    }
    
    if (data && len > 0) {
        send(ctx->client_fd, data, len, 0);
        http_srv.stats.bytes_sent += len;
    }
    
    return 0;
}

static int resp_send_json(response_ctx_t *ctx, const char *json)
{
    ctx->content_type = HTTP_CONTENT_JSON;
    return resp_send(ctx, json, strlen(json));
}

/*===========================================================================*/
/* Request Handling                                                          */
/*===========================================================================*/

static http_route_t *find_route(http_method_t method, const char *path)
{
    for (int i = 0; i < http_srv.route_count; i++) {
        if (http_srv.routes[i].method == method &&
            path_matches(http_srv.routes[i].path, path)) {
            return &http_srv.routes[i];
        }
    }
    return NULL;
}

static int handle_request(int client_fd, char *buffer, size_t len)
{
    /* Parse request line */
    char method_str[8] = {0};
    char path[128] = {0};
    char version[16] = {0};
    
    if (sscanf(buffer, "%7s %127s %15s", method_str, path, version) != 3) {
        LOG_WRN("Invalid HTTP request");
        return -1;
    }
    
    http_method_t method = parse_method(method_str);
    
    /* Split path and query */
    char *query = strchr(path, '?');
    if (query) {
        *query = '\0';
        query++;
    }
    
    /* Find body */
    char *body = strstr(buffer, "\r\n\r\n");
    size_t body_len = 0;
    if (body) {
        body += 4;
        body_len = len - (body - buffer);
    }
    
    /* Find Content-Length */
    size_t content_length = 0;
    char *cl = strstr(buffer, "Content-Length:");
    if (cl) {
        content_length = atoi(cl + 15);
    }
    
    LOG_INF("HTTP %s %s", method_str, path);
    
    /* Build request struct */
    http_request_t req = {
        .method = method,
        .path = path,
        .query = query,
        .body = body,
        .body_len = body_len,
        .content_length = content_length,
    };
    
    /* Build response context */
    response_ctx_t resp_ctx = {
        .client_fd = client_fd,
        .headers_sent = false,
        .status_code = 200,
        .content_type = HTTP_CONTENT_HTML,
    };
    
    http_response_t res = {
        .status_code = 200,
        .content_type = HTTP_CONTENT_HTML,
    };
    
    /* Find matching route */
    http_route_t *route = find_route(method, path);
    
    if (route) {
        int ret = route->handler(&req, &res, route->user_data);
        resp_ctx.status_code = res.status_code;
        resp_ctx.content_type = res.content_type;
        
        if (ret != 0 && !resp_ctx.headers_sent) {
            resp_ctx.status_code = 500;
            resp_send(&resp_ctx, "Internal Server Error", 21);
        }
    } else {
        /* Check for upload path */
        if (http_srv.upload_path && http_srv.upload_cb &&
            strcmp(path, http_srv.upload_path) == 0 && method == HTTP_POST) {
            
            /* Stream upload */
            if (body && body_len > 0) {
                http_srv.upload_cb((uint8_t *)body, body_len, 0, 
                                   content_length, http_srv.upload_user_data);
            }
            
            resp_ctx.status_code = 200;
            resp_ctx.content_type = HTTP_CONTENT_JSON;
            resp_send(&resp_ctx, "{\"status\":\"ok\"}", 15);
        } else {
            /* 404 Not Found */
            resp_ctx.status_code = 404;
            resp_send(&resp_ctx, "Not Found", 9);
        }
    }
    
    http_srv.stats.requests_handled++;
    
    return 0;
}

/*===========================================================================*/
/* Server Thread                                                             */
/*===========================================================================*/

static void server_thread_fn(void *p1, void *p2, void *p3)
{
    struct sockaddr_in addr;
    int ret;
    
    /* Create socket */
    http_srv.server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (http_srv.server_fd < 0) {
        LOG_ERR("Failed to create socket: %d", errno);
        http_srv.state = HTTP_SERVER_ERROR;
        return;
    }
    
    /* Allow address reuse */
    int optval = 1;
    setsockopt(http_srv.server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    
    /* Bind */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(HTTP_SERVER_PORT);
    
    ret = bind(http_srv.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        LOG_ERR("Failed to bind socket: %d", errno);
        close(http_srv.server_fd);
        http_srv.state = HTTP_SERVER_ERROR;
        return;
    }
    
    /* Listen */
    ret = listen(http_srv.server_fd, 4);
    if (ret < 0) {
        LOG_ERR("Failed to listen: %d", errno);
        close(http_srv.server_fd);
        http_srv.state = HTTP_SERVER_ERROR;
        return;
    }
    
    LOG_INF("HTTP server listening on port %d", HTTP_SERVER_PORT);
    http_srv.state = HTTP_SERVER_RUNNING;
    http_srv.running = true;
    
    /* Accept loop */
    while (http_srv.running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(http_srv.server_fd, 
                               (struct sockaddr *)&client_addr, 
                               &client_len);
        
        if (client_fd < 0) {
            if (http_srv.running) {
                LOG_WRN("Accept failed: %d", errno);
            }
            continue;
        }
        
        http_srv.stats.active_connections++;
        
        /* Read request */
        char buffer[HTTP_BUFFER_SIZE];
        ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        
        if (received > 0) {
            buffer[received] = '\0';
            http_srv.stats.bytes_received += received;
            handle_request(client_fd, buffer, received);
        }
        
        close(client_fd);
        http_srv.stats.active_connections--;
    }
    
    close(http_srv.server_fd);
    http_srv.state = HTTP_SERVER_STOPPED;
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int akira_http_server_init(void)
{
    if (http_srv.initialized) {
        return 0;
    }
    
    LOG_INF("Initializing HTTP server");
    
    k_mutex_init(&http_srv.mutex);
    memset(&http_srv.stats, 0, sizeof(http_server_stats_t));
    memset(http_srv.ws_client_fds, -1, sizeof(http_srv.ws_client_fds));
    
    http_srv.state = HTTP_SERVER_STOPPED;
    http_srv.initialized = true;
    
    return 0;
}

int akira_http_server_start(void)
{
    if (!http_srv.initialized) {
        return -EINVAL;
    }
    
    if (http_srv.state == HTTP_SERVER_RUNNING) {
        return 0;
    }
    
    http_srv.state = HTTP_SERVER_STARTING;
    
    k_thread_create(&server_thread, server_stack,
                    K_THREAD_STACK_SIZEOF(server_stack),
                    server_thread_fn,
                    NULL, NULL, NULL,
                    SERVER_THREAD_PRIORITY, 0, K_NO_WAIT);
    
    k_thread_name_set(&server_thread, "http_server");
    
    LOG_INF("HTTP server starting...");
    return 0;
}

int akira_http_server_stop(void)
{
    if (http_srv.state != HTTP_SERVER_RUNNING) {
        return 0;
    }
    
    http_srv.running = false;
    
    /* Close server socket to unblock accept() */
    if (http_srv.server_fd >= 0) {
        close(http_srv.server_fd);
        http_srv.server_fd = -1;
    }
    
    /* Wait for thread to exit */
    k_thread_join(&server_thread, K_SECONDS(5));
    
    http_srv.state = HTTP_SERVER_STOPPED;
    LOG_INF("HTTP server stopped");
    
    return 0;
}

http_server_state_t akira_http_server_get_state(void)
{
    return http_srv.state;
}

bool akira_http_server_is_running(void)
{
    return http_srv.state == HTTP_SERVER_RUNNING;
}

int akira_http_register_route(const http_route_t *route)
{
    if (!route || !route->path || !route->handler) {
        return -EINVAL;
    }
    
    k_mutex_lock(&http_srv.mutex, K_FOREVER);
    
    if (http_srv.route_count >= MAX_ROUTES) {
        k_mutex_unlock(&http_srv.mutex);
        return -ENOMEM;
    }
    
    memcpy(&http_srv.routes[http_srv.route_count], route, sizeof(http_route_t));
    http_srv.route_count++;
    
    k_mutex_unlock(&http_srv.mutex);
    
    LOG_INF("Registered route: %s", route->path);
    return 0;
}

int akira_http_unregister_route(http_method_t method, const char *path)
{
    k_mutex_lock(&http_srv.mutex, K_FOREVER);
    
    for (int i = 0; i < http_srv.route_count; i++) {
        if (http_srv.routes[i].method == method &&
            strcmp(http_srv.routes[i].path, path) == 0) {
            /* Shift remaining routes */
            for (int j = i; j < http_srv.route_count - 1; j++) {
                http_srv.routes[j] = http_srv.routes[j + 1];
            }
            http_srv.route_count--;
            k_mutex_unlock(&http_srv.mutex);
            return 0;
        }
    }
    
    k_mutex_unlock(&http_srv.mutex);
    return -ENOENT;
}

int akira_http_register_upload_handler(const char *path, upload_chunk_cb_t callback,
                                       void *user_data)
{
    http_srv.upload_path = path;
    http_srv.upload_cb = callback;
    http_srv.upload_user_data = user_data;
    return 0;
}

int akira_http_set_static_dir(const char *path)
{
    /* TODO: Implement static file serving */
    return 0;
}

void akira_http_notify_network(bool connected, const char *ip_address)
{
    if (connected && ip_address) {
        strncpy(http_srv.stats.server_ip, ip_address, sizeof(http_srv.stats.server_ip) - 1);
        LOG_INF("Network connected: %s", ip_address);
    } else {
        http_srv.stats.server_ip[0] = '\0';
        LOG_INF("Network disconnected");
    }
}

int akira_http_get_stats(http_server_stats_t *stats)
{
    if (!stats) {
        return -EINVAL;
    }
    
    k_mutex_lock(&http_srv.mutex, K_FOREVER);
    memcpy(stats, &http_srv.stats, sizeof(http_server_stats_t));
    stats->state = http_srv.state;
    stats->ws_clients = akira_http_ws_client_count();
    k_mutex_unlock(&http_srv.mutex);
    
    return 0;
}

/*===========================================================================*/
/* WebSocket API                                                             */
/*===========================================================================*/

int akira_http_enable_websocket(void)
{
    http_srv.ws_enabled = true;
    LOG_INF("WebSocket support enabled");
    return 0;
}

int akira_http_ws_register_message_cb(ws_message_cb_t callback, void *user_data)
{
    http_srv.ws_msg_cb = callback;
    http_srv.ws_msg_user_data = user_data;
    return 0;
}

int akira_http_ws_register_event_cb(ws_event_cb_t callback, void *user_data)
{
    http_srv.ws_event_cb = callback;
    http_srv.ws_event_user_data = user_data;
    return 0;
}

int akira_http_ws_send(int client_id, const uint8_t *data, size_t len)
{
    if (!http_srv.ws_enabled) {
        return -ENOTSUP;
    }
    
    /* TODO: Implement WebSocket frame encoding and sending */
    
    if (client_id < 0) {
        /* Broadcast to all clients */
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (http_srv.ws_client_fds[i] >= 0) {
                send(http_srv.ws_client_fds[i], data, len, 0);
            }
        }
    } else if (client_id < MAX_WS_CLIENTS && http_srv.ws_client_fds[client_id] >= 0) {
        send(http_srv.ws_client_fds[client_id], data, len, 0);
    }
    
    return 0;
}

int akira_http_ws_send_text(int client_id, const char *text)
{
    return akira_http_ws_send(client_id, (const uint8_t *)text, strlen(text));
}

int akira_http_ws_disconnect(int client_id)
{
    if (client_id < 0 || client_id >= MAX_WS_CLIENTS) {
        return -EINVAL;
    }
    
    if (http_srv.ws_client_fds[client_id] >= 0) {
        close(http_srv.ws_client_fds[client_id]);
        http_srv.ws_client_fds[client_id] = -1;
    }
    
    return 0;
}

int akira_http_ws_client_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (http_srv.ws_client_fds[i] >= 0) {
            count++;
        }
    }
    return count;
}
