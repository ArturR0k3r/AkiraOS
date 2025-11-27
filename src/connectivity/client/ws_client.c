/**
 * @file ws_client.c
 * @brief WebSocket Client Implementation for AkiraOS
 */

#include "ws_client.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/websocket.h>
#include <zephyr/net/http/client.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(ws_client, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define WS_CLIENT_THREAD_STACK_SIZE 2048
#define WS_CLIENT_THREAD_PRIORITY 8

/*===========================================================================*/
/* Internal Types                                                            */
/*===========================================================================*/

struct ws_client_conn
{
    bool in_use;
    ws_client_state_t state;
    ws_client_config_t config;
    char url[WS_CLIENT_MAX_URL_LEN];

    /* Socket */
    int sock_fd;
    int websock_fd;

    /* Callbacks */
    ws_client_message_cb_t msg_cb;
    void *msg_user_data;
    ws_client_event_cb_t event_cb;
    void *event_user_data;

    /* Buffers */
    uint8_t rx_buffer[WS_CLIENT_RX_BUFFER_SIZE];
    uint8_t tx_buffer[WS_CLIENT_TX_BUFFER_SIZE];

    /* Thread */
    struct k_thread thread;
    k_tid_t thread_id;
    bool running;

    /* Ping/pong */
    int64_t last_ping_time;
    int64_t last_pong_time;
};

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    struct ws_client_conn connections[WS_CLIENT_MAX_CONNECTIONS];
    struct k_mutex mutex;
} ws_client_state;

static K_THREAD_STACK_ARRAY_DEFINE(ws_thread_stacks, WS_CLIENT_MAX_CONNECTIONS,
                                   WS_CLIENT_THREAD_STACK_SIZE);

/*===========================================================================*/
/* Forward Declarations                                                      */
/*===========================================================================*/

static void ws_client_thread_fn(void *p1, void *p2, void *p3);
static int parse_ws_url(const char *url, char *host, size_t host_len,
                        uint16_t *port, char *path, size_t path_len, bool *use_tls);

/*===========================================================================*/
/* Initialization                                                            */
/*===========================================================================*/

int ws_client_init(void)
{
    if (ws_client_state.initialized)
    {
        return 0;
    }

    LOG_INF("Initializing WebSocket client");

    k_mutex_init(&ws_client_state.mutex);
    memset(ws_client_state.connections, 0, sizeof(ws_client_state.connections));

    ws_client_state.initialized = true;
    return 0;
}

int ws_client_deinit(void)
{
    if (!ws_client_state.initialized)
    {
        return 0;
    }

    /* Close all connections */
    for (int i = 0; i < WS_CLIENT_MAX_CONNECTIONS; i++)
    {
        if (ws_client_state.connections[i].in_use)
        {
            ws_client_disconnect(i, 1000, "Shutdown");
        }
    }

    ws_client_state.initialized = false;
    return 0;
}

/*===========================================================================*/
/* Connection Management                                                     */
/*===========================================================================*/

ws_client_handle_t ws_client_connect(const ws_client_config_t *config)
{
    if (!ws_client_state.initialized || !config || !config->url)
    {
        return -EINVAL;
    }

    k_mutex_lock(&ws_client_state.mutex, K_FOREVER);

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < WS_CLIENT_MAX_CONNECTIONS; i++)
    {
        if (!ws_client_state.connections[i].in_use)
        {
            slot = i;
            break;
        }
    }

    if (slot < 0)
    {
        k_mutex_unlock(&ws_client_state.mutex);
        LOG_ERR("No free connection slots");
        return -ENOMEM;
    }

    struct ws_client_conn *conn = &ws_client_state.connections[slot];
    memset(conn, 0, sizeof(*conn));

    conn->in_use = true;
    conn->state = WS_CLIENT_CONNECTING;
    conn->sock_fd = -1;
    conn->websock_fd = -1;

    /* Copy config */
    strncpy(conn->url, config->url, WS_CLIENT_MAX_URL_LEN - 1);
    conn->config = *config;
    conn->config.url = conn->url;

    /* Set defaults */
    if (conn->config.connect_timeout_ms == 0)
    {
        conn->config.connect_timeout_ms = 10000;
    }

    k_mutex_unlock(&ws_client_state.mutex);

    /* Start connection thread */
    conn->running = true;
    conn->thread_id = k_thread_create(&conn->thread,
                                      ws_thread_stacks[slot],
                                      WS_CLIENT_THREAD_STACK_SIZE,
                                      ws_client_thread_fn,
                                      conn, NULL, NULL,
                                      WS_CLIENT_THREAD_PRIORITY, 0, K_NO_WAIT);

    char name[16];
    snprintf(name, sizeof(name), "ws_client_%d", slot);
    k_thread_name_set(conn->thread_id, name);

    LOG_INF("WebSocket client connecting to %s", conn->url);
    return slot;
}

int ws_client_disconnect(ws_client_handle_t handle, uint16_t code, const char *reason)
{
    if (handle < 0 || handle >= WS_CLIENT_MAX_CONNECTIONS)
    {
        return -EINVAL;
    }

    struct ws_client_conn *conn = &ws_client_state.connections[handle];

    if (!conn->in_use)
    {
        return -ENOENT;
    }

    conn->state = WS_CLIENT_CLOSING;
    conn->running = false;

    /* Send close frame if connected */
    if (conn->websock_fd >= 0)
    {
#ifdef CONFIG_WEBSOCKET_CLIENT
        websocket_send_msg(conn->websock_fd, NULL, 0,
                           WEBSOCKET_OPCODE_CLOSE, true, true, K_MSEC(1000));
#endif
    }

    /* Close sockets */
    if (conn->websock_fd >= 0)
    {
        close(conn->websock_fd);
        conn->websock_fd = -1;
    }
    if (conn->sock_fd >= 0)
    {
        close(conn->sock_fd);
        conn->sock_fd = -1;
    }

    /* Wait for thread */
    if (conn->thread_id)
    {
        k_thread_join(&conn->thread, K_SECONDS(2));
    }

    /* Notify disconnect */
    if (conn->event_cb)
    {
        conn->event_cb(handle, WS_CLIENT_EVENT_DISCONNECTED, NULL, conn->event_user_data);
    }

    conn->state = WS_CLIENT_DISCONNECTED;
    conn->in_use = false;

    LOG_INF("WebSocket client disconnected");
    return 0;
}

ws_client_state_t ws_client_get_state(ws_client_handle_t handle)
{
    if (handle < 0 || handle >= WS_CLIENT_MAX_CONNECTIONS)
    {
        return WS_CLIENT_ERROR;
    }

    return ws_client_state.connections[handle].state;
}

bool ws_client_is_connected(ws_client_handle_t handle)
{
    return ws_client_get_state(handle) == WS_CLIENT_CONNECTED;
}

/*===========================================================================*/
/* Sending Data                                                              */
/*===========================================================================*/

int ws_client_send_text(ws_client_handle_t handle, const char *text)
{
    if (handle < 0 || handle >= WS_CLIENT_MAX_CONNECTIONS || !text)
    {
        return -EINVAL;
    }

    struct ws_client_conn *conn = &ws_client_state.connections[handle];

    if (conn->state != WS_CLIENT_CONNECTED)
    {
        return -ENOTCONN;
    }

#ifdef CONFIG_WEBSOCKET_CLIENT
    int ret = websocket_send_msg(conn->websock_fd,
                                 (const uint8_t *)text, strlen(text),
                                 WEBSOCKET_OPCODE_DATA_TEXT, true, true,
                                 K_MSEC(5000));
    if (ret < 0)
    {
        LOG_ERR("Failed to send text: %d", ret);
        return ret;
    }
#else
    /* Fallback: send raw (for simulation) */
    if (conn->sock_fd >= 0)
    {
        send(conn->sock_fd, text, strlen(text), 0);
    }
#endif

    return 0;
}

int ws_client_send_binary(ws_client_handle_t handle, const uint8_t *data, size_t len)
{
    if (handle < 0 || handle >= WS_CLIENT_MAX_CONNECTIONS || !data)
    {
        return -EINVAL;
    }

    struct ws_client_conn *conn = &ws_client_state.connections[handle];

    if (conn->state != WS_CLIENT_CONNECTED)
    {
        return -ENOTCONN;
    }

#ifdef CONFIG_WEBSOCKET_CLIENT
    int ret = websocket_send_msg(conn->websock_fd, data, len,
                                 WEBSOCKET_OPCODE_DATA_BINARY, true, true,
                                 K_MSEC(5000));
    if (ret < 0)
    {
        LOG_ERR("Failed to send binary: %d", ret);
        return ret;
    }
#else
    if (conn->sock_fd >= 0)
    {
        send(conn->sock_fd, data, len, 0);
    }
#endif

    return 0;
}

int ws_client_send_ping(ws_client_handle_t handle)
{
    if (handle < 0 || handle >= WS_CLIENT_MAX_CONNECTIONS)
    {
        return -EINVAL;
    }

    struct ws_client_conn *conn = &ws_client_state.connections[handle];

    if (conn->state != WS_CLIENT_CONNECTED)
    {
        return -ENOTCONN;
    }

#ifdef CONFIG_WEBSOCKET_CLIENT
    int ret = websocket_send_msg(conn->websock_fd, NULL, 0,
                                 WEBSOCKET_OPCODE_PING, true, true,
                                 K_MSEC(1000));
    if (ret < 0)
    {
        return ret;
    }
    conn->last_ping_time = k_uptime_get();
#endif

    return 0;
}

/*===========================================================================*/
/* Callbacks                                                                 */
/*===========================================================================*/

int ws_client_set_message_cb(ws_client_handle_t handle,
                             ws_client_message_cb_t callback,
                             void *user_data)
{
    if (handle < 0 || handle >= WS_CLIENT_MAX_CONNECTIONS)
    {
        return -EINVAL;
    }

    struct ws_client_conn *conn = &ws_client_state.connections[handle];
    conn->msg_cb = callback;
    conn->msg_user_data = user_data;

    return 0;
}

int ws_client_set_event_cb(ws_client_handle_t handle,
                           ws_client_event_cb_t callback,
                           void *user_data)
{
    if (handle < 0 || handle >= WS_CLIENT_MAX_CONNECTIONS)
    {
        return -EINVAL;
    }

    struct ws_client_conn *conn = &ws_client_state.connections[handle];
    conn->event_cb = callback;
    conn->event_user_data = user_data;

    return 0;
}

/*===========================================================================*/
/* Utility                                                                   */
/*===========================================================================*/

int ws_client_set_header(ws_client_handle_t handle, const char *name, const char *value)
{
    /* TODO: Store custom headers for handshake */
    (void)handle;
    (void)name;
    (void)value;
    return 0;
}

const char *ws_client_get_url(ws_client_handle_t handle)
{
    if (handle < 0 || handle >= WS_CLIENT_MAX_CONNECTIONS)
    {
        return NULL;
    }

    return ws_client_state.connections[handle].url;
}

/*===========================================================================*/
/* URL Parsing                                                               */
/*===========================================================================*/

static int parse_ws_url(const char *url, char *host, size_t host_len,
                        uint16_t *port, char *path, size_t path_len, bool *use_tls)
{
    if (!url || !host || !port || !path || !use_tls)
    {
        return -EINVAL;
    }

    *use_tls = false;
    *port = 80;

    const char *p = url;

    /* Parse scheme */
    if (strncmp(p, "wss://", 6) == 0)
    {
        *use_tls = true;
        *port = 443;
        p += 6;
    }
    else if (strncmp(p, "ws://", 5) == 0)
    {
        p += 5;
    }
    else
    {
        return -EINVAL;
    }

    /* Parse host */
    const char *host_start = p;
    const char *host_end = NULL;

    while (*p && *p != ':' && *p != '/')
    {
        p++;
    }
    host_end = p;

    size_t len = host_end - host_start;
    if (len >= host_len)
    {
        return -ENOMEM;
    }
    memcpy(host, host_start, len);
    host[len] = '\0';

    /* Parse port */
    if (*p == ':')
    {
        p++;
        *port = atoi(p);
        while (*p && *p != '/')
        {
            p++;
        }
    }

    /* Parse path */
    if (*p == '/')
    {
        strncpy(path, p, path_len - 1);
        path[path_len - 1] = '\0';
    }
    else
    {
        strncpy(path, "/", path_len - 1);
    }

    return 0;
}

/*===========================================================================*/
/* Connection Thread                                                         */
/*===========================================================================*/

static void ws_client_thread_fn(void *p1, void *p2, void *p3)
{
    struct ws_client_conn *conn = (struct ws_client_conn *)p1;
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    char host[128];
    char path[128];
    uint16_t port;
    bool use_tls;

    /* Parse URL */
    int ret = parse_ws_url(conn->url, host, sizeof(host),
                           &port, path, sizeof(path), &use_tls);
    if (ret < 0)
    {
        LOG_ERR("Invalid WebSocket URL: %s", conn->url);
        conn->state = WS_CLIENT_ERROR;
        if (conn->event_cb)
        {
            conn->event_cb(-1, WS_CLIENT_EVENT_ERROR, NULL, conn->event_user_data);
        }
        return;
    }

    LOG_DBG("Connecting to %s:%d%s (TLS=%d)", host, port, path, use_tls);

    /* Resolve address */
    struct zsock_addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM};
    struct zsock_addrinfo *addr_res;

    ret = zsock_getaddrinfo(host, NULL, &hints, &addr_res);
    if (ret != 0)
    {
        LOG_ERR("DNS resolution failed for %s", host);
        conn->state = WS_CLIENT_ERROR;
        return;
    }

    struct sockaddr_in *addr4 = (struct sockaddr_in *)addr_res->ai_addr;
    addr4->sin_port = htons(port);

    /* Create socket */
    conn->sock_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (conn->sock_fd < 0)
    {
        LOG_ERR("Socket creation failed");
        zsock_freeaddrinfo(addr_res);
        conn->state = WS_CLIENT_ERROR;
        return;
    }

    /* Connect */
    ret = zsock_connect(conn->sock_fd, (struct sockaddr *)addr4,
                        sizeof(struct sockaddr_in));
    zsock_freeaddrinfo(addr_res);

    if (ret < 0)
    {
        LOG_ERR("TCP connect failed: %d", errno);
        close(conn->sock_fd);
        conn->sock_fd = -1;
        conn->state = WS_CLIENT_ERROR;
        return;
    }

#ifdef CONFIG_WEBSOCKET_CLIENT
    /* WebSocket handshake */
    struct websocket_request ws_req = {
        .host = host,
        .url = path,
        .cb = NULL,
        .tmp_buf = conn->rx_buffer,
        .tmp_buf_len = sizeof(conn->rx_buffer)};

    conn->websock_fd = websocket_connect(conn->sock_fd, &ws_req,
                                         K_MSEC(conn->config.connect_timeout_ms),
                                         NULL);
    if (conn->websock_fd < 0)
    {
        LOG_ERR("WebSocket handshake failed: %d", conn->websock_fd);
        close(conn->sock_fd);
        conn->sock_fd = -1;
        conn->state = WS_CLIENT_ERROR;
        return;
    }
#else
    conn->websock_fd = conn->sock_fd;
#endif

    conn->state = WS_CLIENT_CONNECTED;
    LOG_INF("WebSocket connected to %s", conn->url);

    /* Notify connected */
    if (conn->event_cb)
    {
        conn->event_cb(-1, WS_CLIENT_EVENT_CONNECTED, NULL, conn->event_user_data);
    }

    /* Receive loop */
    while (conn->running && conn->state == WS_CLIENT_CONNECTED)
    {
#ifdef CONFIG_WEBSOCKET_CLIENT
        uint32_t msg_type;
        uint64_t remaining = 0;

        ret = websocket_recv_msg(conn->websock_fd, conn->rx_buffer,
                                 sizeof(conn->rx_buffer) - 1,
                                 &msg_type, &remaining, K_MSEC(100));

        if (ret > 0)
        {
            conn->rx_buffer[ret] = '\0';

            ws_msg_type_t type = WS_MSG_TEXT;
            if (msg_type & WEBSOCKET_OPCODE_DATA_BINARY)
            {
                type = WS_MSG_BINARY;
            }
            else if (msg_type & WEBSOCKET_OPCODE_PONG)
            {
                type = WS_MSG_PONG;
                conn->last_pong_time = k_uptime_get();
            }
            else if (msg_type & WEBSOCKET_OPCODE_PING)
            {
                type = WS_MSG_PING;
                /* Auto-respond with pong */
                websocket_send_msg(conn->websock_fd, conn->rx_buffer, ret,
                                   WEBSOCKET_OPCODE_PONG, true, true, K_MSEC(1000));
            }

            if (conn->msg_cb)
            {
                conn->msg_cb(-1, type, conn->rx_buffer, ret, conn->msg_user_data);
            }
        }
        else if (ret == -EAGAIN || ret == -EWOULDBLOCK)
        {
            /* Timeout, check ping interval */
            if (conn->config.ping_interval_ms > 0)
            {
                int64_t now = k_uptime_get();
                if (now - conn->last_ping_time > conn->config.ping_interval_ms)
                {
                    ws_client_send_ping(-1);
                }
            }
        }
        else if (ret < 0 && ret != -EAGAIN)
        {
            LOG_WRN("WebSocket receive error: %d", ret);
            break;
        }
#else
        /* Simulation mode: simple socket receive */
        struct zsock_pollfd pfd = {
            .fd = conn->sock_fd,
            .events = ZSOCK_POLLIN};

        ret = zsock_poll(&pfd, 1, 100);
        if (ret > 0 && (pfd.revents & ZSOCK_POLLIN))
        {
            ret = recv(conn->sock_fd, conn->rx_buffer,
                       sizeof(conn->rx_buffer) - 1, 0);
            if (ret > 0)
            {
                conn->rx_buffer[ret] = '\0';
                if (conn->msg_cb)
                {
                    conn->msg_cb(-1, WS_MSG_TEXT, conn->rx_buffer, ret,
                                 conn->msg_user_data);
                }
            }
            else if (ret == 0)
            {
                LOG_INF("Server closed connection");
                break;
            }
        }
#endif
    }

    /* Cleanup */
    if (conn->websock_fd >= 0)
    {
        close(conn->websock_fd);
        conn->websock_fd = -1;
    }
    if (conn->sock_fd >= 0 && conn->sock_fd != conn->websock_fd)
    {
        close(conn->sock_fd);
        conn->sock_fd = -1;
    }

    if (conn->state != WS_CLIENT_CLOSING)
    {
        conn->state = WS_CLIENT_DISCONNECTED;

        /* Notify disconnect */
        if (conn->event_cb)
        {
            conn->event_cb(-1, WS_CLIENT_EVENT_DISCONNECTED, NULL, conn->event_user_data);
        }

        /* Auto-reconnect if enabled */
        if (conn->config.auto_reconnect && conn->running)
        {
            LOG_INF("Auto-reconnecting in %d ms", conn->config.reconnect_delay_ms);
            k_msleep(conn->config.reconnect_delay_ms);
            /* Recursive reconnect - TODO: implement properly */
        }
    }
}
