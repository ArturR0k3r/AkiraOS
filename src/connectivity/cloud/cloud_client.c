/**
 * @file cloud_client.c
 * @brief Unified Cloud Client Implementation
 *
 * Handles communication from all sources:
 * - Remote servers (WebSocket)
 * - AkiraApp (Bluetooth)
 * - Local web server
 */

#include "cloud_client.h"
#include "cloud_protocol.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(cloud_client, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define WORKER_STACK_SIZE 4096
#define WORKER_PRIORITY 5
#define RX_QUEUE_ALIGN 4

/*===========================================================================*/
/* Private Types                                                             */
/*===========================================================================*/

/** Handler entry */
struct handler_entry
{
    msg_category_t category;
    cloud_msg_handler_t handler;
};

/** Transfer state for OTA/App downloads */
struct transfer_state
{
    bool active;
    msg_type_t type;
    msg_source_t source;
    char app_id[32];
    uint32_t total_size;
    uint32_t received;
    uint16_t expected_chunks;
    uint16_t received_chunks;
    uint8_t hash[32];
};

/** Message queue entry */
struct rx_msg_entry
{
    void *fifo_reserved;
    cloud_message_t msg;
    msg_source_t source;
};

/*===========================================================================*/
/* Private Data                                                              */
/*===========================================================================*/

static struct
{
    bool initialized;
    cloud_client_config_t config;

    /* Connection states */
    cloud_state_t cloud_state;
    cloud_state_t bt_state;
    cloud_state_t web_state;

    /* Handlers */
    struct handler_entry handlers[CLOUD_CLIENT_MAX_HANDLERS];
    int handler_count;
    cloud_state_handler_t state_handler;
    cloud_ota_data_handler_t ota_handler;
    cloud_app_data_handler_t app_data_handler;
    cloud_app_complete_handler_t app_complete_handler;

    /* Active transfers */
    struct transfer_state ota_transfer;
    struct transfer_state app_transfer;

    /* Statistics */
    cloud_client_stats_t stats;

    /* Synchronization */
    struct k_mutex mutex;
    struct k_fifo rx_queue;

    /* Worker thread */
    struct k_thread worker_thread;
    bool worker_running;

    /* Heartbeat timer */
    struct k_timer heartbeat_timer;

} client;

/* Worker thread stack - must be outside struct */
static K_THREAD_STACK_DEFINE(worker_stack, WORKER_STACK_SIZE);

/*===========================================================================*/
/* Forward Declarations                                                      */
/*===========================================================================*/

static void worker_entry(void *p1, void *p2, void *p3);
static void process_message(const cloud_message_t *msg, msg_source_t source);
static void handle_ota_message(const cloud_message_t *msg, msg_source_t source);
static void handle_app_message(const cloud_message_t *msg, msg_source_t source);
static void heartbeat_handler(struct k_timer *timer);
static int send_via_transport(const uint8_t *data, size_t len, msg_source_t dest);

/*===========================================================================*/
/* Initialization                                                            */
/*===========================================================================*/

int cloud_client_init(const cloud_client_config_t *config)
{
    if (client.initialized)
    {
        return 0;
    }

    memset(&client, 0, sizeof(client));

    /* Apply configuration */
    if (config)
    {
        memcpy(&client.config, config, sizeof(*config));
    }
    else
    {
        /* Defaults */
        client.config.auto_reconnect = true;
        client.config.reconnect_delay_ms = 5000;
        client.config.heartbeat_interval_ms = 30000;
        client.config.enable_bluetooth = true;
        client.config.enable_webserver = true;
    }

    k_mutex_init(&client.mutex);
    k_fifo_init(&client.rx_queue);

    /* Initialize heartbeat timer */
    k_timer_init(&client.heartbeat_timer, heartbeat_handler, NULL);

    /* Start worker thread */
    client.worker_running = true;
    k_thread_create(&client.worker_thread, worker_stack,
                    K_THREAD_STACK_SIZEOF(worker_stack), worker_entry,
                    NULL, NULL, NULL,
                    WORKER_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&client.worker_thread, "cloud_worker");

    /* Start heartbeat if configured */
    if (client.config.heartbeat_interval_ms > 0)
    {
        k_timer_start(&client.heartbeat_timer,
                      K_MSEC(client.config.heartbeat_interval_ms),
                      K_MSEC(client.config.heartbeat_interval_ms));
    }

    client.initialized = true;
    LOG_INF("Cloud client initialized");

    /* Auto-connect if configured */
    if (client.config.auto_connect && client.config.server_url)
    {
        cloud_client_connect(client.config.server_url);
    }

    return 0;
}

int cloud_client_deinit(void)
{
    if (!client.initialized)
    {
        return 0;
    }

    /* Stop heartbeat */
    k_timer_stop(&client.heartbeat_timer);

    /* Stop worker */
    client.worker_running = false;
    k_thread_abort(&client.worker_thread);

    /* Disconnect */
    cloud_client_disconnect();

    client.initialized = false;
    LOG_INF("Cloud client deinitialized");

    return 0;
}

bool cloud_client_is_initialized(void)
{
    return client.initialized;
}

/*===========================================================================*/
/* Connection Management                                                     */
/*===========================================================================*/

int cloud_client_connect(const char *url)
{
    if (!client.initialized)
    {
        return -EINVAL;
    }

    const char *target_url = url ? url : client.config.server_url;
    if (!target_url)
    {
        LOG_ERR("No server URL configured");
        return -EINVAL;
    }

    LOG_INF("Connecting to %s", target_url);
    client.cloud_state = CLOUD_STATE_CONNECTING;

    if (client.state_handler)
    {
        client.state_handler(MSG_SOURCE_CLOUD, CLOUD_STATE_CONNECTING);
    }

    /* TODO: Actually connect via WebSocket/CoAP/MQTT */
    /* For now, simulate connection */
    client.cloud_state = CLOUD_STATE_CONNECTED;

    if (client.state_handler)
    {
        client.state_handler(MSG_SOURCE_CLOUD, CLOUD_STATE_CONNECTED);
    }

    LOG_INF("Connected to cloud");
    return 0;
}

int cloud_client_disconnect(void)
{
    if (!client.initialized)
    {
        return -EINVAL;
    }

    client.cloud_state = CLOUD_STATE_DISCONNECTED;

    if (client.state_handler)
    {
        client.state_handler(MSG_SOURCE_CLOUD, CLOUD_STATE_DISCONNECTED);
    }

    LOG_INF("Disconnected from cloud");
    return 0;
}

cloud_state_t cloud_client_get_state(msg_source_t source)
{
    switch (source)
    {
    case MSG_SOURCE_CLOUD:
        return client.cloud_state;
    case MSG_SOURCE_BT_APP:
        return client.bt_state;
    case MSG_SOURCE_WEB_SERVER:
        return client.web_state;
    default:
        return CLOUD_STATE_DISCONNECTED;
    }
}

int cloud_client_get_sources(cloud_source_info_t *info, int max_count)
{
    if (!info || max_count < 1)
    {
        return -EINVAL;
    }

    int count = 0;

    /* Cloud source */
    if (count < max_count)
    {
        info[count].source = MSG_SOURCE_CLOUD;
        info[count].transport = CLOUD_TRANSPORT_WEBSOCKET;
        info[count].state = client.cloud_state;
        info[count].authenticated = (client.cloud_state == CLOUD_STATE_AUTHENTICATED);
        count++;
    }

    /* Bluetooth source */
    if (count < max_count && client.config.enable_bluetooth)
    {
        info[count].source = MSG_SOURCE_BT_APP;
        info[count].transport = CLOUD_TRANSPORT_BLE;
        info[count].state = client.bt_state;
        info[count].authenticated = true; /* BLE pairing = auth */
        count++;
    }

    /* Web server source */
    if (count < max_count && client.config.enable_webserver)
    {
        info[count].source = MSG_SOURCE_WEB_SERVER;
        info[count].transport = CLOUD_TRANSPORT_HTTP;
        info[count].state = client.web_state;
        info[count].authenticated = true;
        count++;
    }

    return count;
}

/*===========================================================================*/
/* Handler Registration                                                      */
/*===========================================================================*/

int cloud_client_register_handler(msg_category_t category, cloud_msg_handler_t handler)
{
    if (!handler)
    {
        return -EINVAL;
    }

    k_mutex_lock(&client.mutex, K_FOREVER);

    if (client.handler_count >= CLOUD_CLIENT_MAX_HANDLERS)
    {
        k_mutex_unlock(&client.mutex);
        return -ENOMEM;
    }

    client.handlers[client.handler_count].category = category;
    client.handlers[client.handler_count].handler = handler;
    client.handler_count++;

    k_mutex_unlock(&client.mutex);

    LOG_DBG("Registered handler for category 0x%02X", category);
    return 0;
}

int cloud_client_register_state_handler(cloud_state_handler_t handler)
{
    client.state_handler = handler;
    return 0;
}

int cloud_client_register_ota_handler(cloud_ota_data_handler_t handler)
{
    client.ota_handler = handler;
    return 0;
}

int cloud_client_register_app_handler(cloud_app_data_handler_t data_handler,
                                      cloud_app_complete_handler_t complete_handler)
{
    client.app_data_handler = data_handler;
    client.app_complete_handler = complete_handler;
    return 0;
}

/*===========================================================================*/
/* Sending Messages                                                          */
/*===========================================================================*/

int cloud_client_send(const cloud_message_t *msg, msg_source_t dest)
{
    if (!client.initialized || !msg)
    {
        return -EINVAL;
    }

    uint8_t buffer[CLOUD_MSG_MAX_SIZE];
    int len = cloud_msg_serialize(msg, buffer, sizeof(buffer));
    if (len < 0)
    {
        LOG_ERR("Failed to serialize message");
        return len;
    }

    int ret = send_via_transport(buffer, len, dest);
    if (ret == 0)
    {
        client.stats.total_messages_tx++;
        client.stats.total_bytes_tx += len;
    }

    return ret;
}

int cloud_client_broadcast(const cloud_message_t *msg)
{
    int success_count = 0;

    if (client.cloud_state >= CLOUD_STATE_CONNECTED)
    {
        if (cloud_client_send(msg, MSG_SOURCE_CLOUD) == 0)
        {
            success_count++;
        }
    }

    if (client.bt_state >= CLOUD_STATE_CONNECTED)
    {
        if (cloud_client_send(msg, MSG_SOURCE_BT_APP) == 0)
        {
            success_count++;
        }
    }

    if (client.web_state >= CLOUD_STATE_CONNECTED)
    {
        if (cloud_client_send(msg, MSG_SOURCE_WEB_SERVER) == 0)
        {
            success_count++;
        }
    }

    return success_count;
}

int cloud_client_send_raw(const uint8_t *data, size_t len, msg_source_t dest)
{
    if (!client.initialized || !data)
    {
        return -EINVAL;
    }

    return send_via_transport(data, len, dest);
}

/*===========================================================================*/
/* High-Level Operations                                                     */
/*===========================================================================*/

int cloud_client_send_status(msg_source_t dest)
{
    cloud_message_t msg;
    payload_status_t status = {0};

    /* Fill status */
    status.fw_version[0] = 2; /* Major */
    status.fw_version[1] = 0; /* Minor */
    status.fw_version[2] = 0; /* Patch */
    status.uptime_sec = (uint32_t)(k_uptime_get() / 1000);
    status.battery_mv = 3700; /* TODO: Real battery reading */
    status.battery_pct = 85;
    status.cpu_usage = 0;   /* TODO: Real CPU usage */
    status.free_memory = 0; /* TODO: Get actual free memory */

    /* Build message */
    cloud_msg_init(&msg.header, MSG_TYPE_STATUS_RESPONSE, MSG_SOURCE_INTERNAL);
    msg.header.flags |= MSG_FLAG_RESPONSE;
    msg.header.payload_len = sizeof(status);
    msg.payload = (uint8_t *)&status;

    if (dest == 0)
    {
        return cloud_client_broadcast(&msg);
    }
    return cloud_client_send(&msg, dest);
}

int cloud_client_check_firmware(void)
{
    cloud_message_t msg;

    cloud_msg_init(&msg.header, MSG_TYPE_FW_CHECK, MSG_SOURCE_INTERNAL);
    msg.payload = NULL;

    return cloud_client_send(&msg, MSG_SOURCE_CLOUD);
}

int cloud_client_request_firmware(const char *version)
{
    cloud_message_t msg;

    cloud_msg_init(&msg.header, MSG_TYPE_FW_REQUEST, MSG_SOURCE_INTERNAL);

    if (version)
    {
        msg.header.payload_len = strlen(version) + 1;
        msg.payload = (uint8_t *)version;
    }
    else
    {
        msg.payload = NULL;
    }

    /* Request can come from any connected source */
    return cloud_client_broadcast(&msg);
}

int cloud_client_request_app_list(void)
{
    cloud_message_t msg;

    cloud_msg_init(&msg.header, MSG_TYPE_APP_LIST_REQUEST, MSG_SOURCE_INTERNAL);
    msg.payload = NULL;

    return cloud_client_broadcast(&msg);
}

int cloud_client_request_app(const char *app_id)
{
    if (!app_id)
    {
        return -EINVAL;
    }

    cloud_message_t msg;

    cloud_msg_init(&msg.header, MSG_TYPE_APP_REQUEST, MSG_SOURCE_INTERNAL);
    msg.header.payload_len = strlen(app_id) + 1;
    msg.payload = (uint8_t *)app_id;

    return cloud_client_broadcast(&msg);
}

int cloud_client_check_app_updates(void)
{
    cloud_message_t msg;

    cloud_msg_init(&msg.header, MSG_TYPE_APP_CHECK, MSG_SOURCE_INTERNAL);
    msg.payload = NULL;

    return cloud_client_broadcast(&msg);
}

int cloud_client_send_sensor_data(const uint8_t *data, size_t len)
{
    if (!data || len == 0)
    {
        return -EINVAL;
    }

    cloud_message_t msg;

    cloud_msg_init(&msg.header, MSG_TYPE_SENSOR_DATA, MSG_SOURCE_INTERNAL);
    msg.header.payload_len = len;
    msg.payload = (uint8_t *)data;

    return cloud_client_send(&msg, MSG_SOURCE_CLOUD);
}

int cloud_client_heartbeat(void)
{
    cloud_message_t msg;

    cloud_msg_init(&msg.header, MSG_TYPE_HEARTBEAT, MSG_SOURCE_INTERNAL);
    msg.payload = NULL;

    return cloud_client_broadcast(&msg);
}

/*===========================================================================*/
/* External Interfaces                                                       */
/*===========================================================================*/

int cloud_client_bt_receive(const uint8_t *data, size_t len)
{
    if (!client.initialized || !data)
    {
        return -EINVAL;
    }

    cloud_message_t msg;
    int ret = cloud_msg_parse(data, len, &msg);
    if (ret < 0)
    {
        LOG_ERR("Failed to parse BT message: %d", ret);
        client.stats.errors++;
        return ret;
    }

    client.stats.total_messages_rx++;
    client.stats.total_bytes_rx += len;

    /* Process directly or queue */
    process_message(&msg, MSG_SOURCE_BT_APP);
    cloud_msg_free(&msg);

    return 0;
}

void cloud_client_bt_connected(bool connected)
{
    client.bt_state = connected ? CLOUD_STATE_CONNECTED : CLOUD_STATE_DISCONNECTED;

    if (client.state_handler)
    {
        client.state_handler(MSG_SOURCE_BT_APP, client.bt_state);
    }

    LOG_INF("Bluetooth %s", connected ? "connected" : "disconnected");
}

int cloud_client_ws_receive(const uint8_t *data, size_t len, bool is_binary)
{
    if (!client.initialized || !data)
    {
        return -EINVAL;
    }

    client.stats.total_messages_rx++;
    client.stats.total_bytes_rx += len;

    if (is_binary)
    {
        /* Binary = protocol message */
        cloud_message_t msg;
        int ret = cloud_msg_parse(data, len, &msg);
        if (ret < 0)
        {
            LOG_ERR("Failed to parse WS message: %d", ret);
            client.stats.errors++;
            return ret;
        }

        process_message(&msg, MSG_SOURCE_WEB_SERVER);
        cloud_msg_free(&msg);
    }
    else
    {
        /* Text = JSON, parse accordingly */
        /* TODO: JSON parsing for web interface compatibility */
        LOG_DBG("Received text message, len=%zu", len);
    }

    return 0;
}

/*===========================================================================*/
/* Statistics                                                                */
/*===========================================================================*/

void cloud_client_get_stats(cloud_client_stats_t *stats)
{
    if (stats)
    {
        memcpy(stats, &client.stats, sizeof(*stats));
    }
}

void cloud_client_reset_stats(void)
{
    memset(&client.stats, 0, sizeof(client.stats));
}

/*===========================================================================*/
/* Private Functions                                                         */
/*===========================================================================*/

static void worker_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_DBG("Cloud worker started");

    while (client.worker_running)
    {
        struct rx_msg_entry *entry = k_fifo_get(&client.rx_queue, K_MSEC(100));

        if (entry)
        {
            process_message(&entry->msg, entry->source);
            cloud_msg_free(&entry->msg);
            k_free(entry);
        }

        /* Periodic tasks */
        /* TODO: Check for timeouts, retries, etc. */
    }

    LOG_DBG("Cloud worker stopped");
}

static void process_message(const cloud_message_t *msg, msg_source_t source)
{
    LOG_DBG("Processing %s from %s",
            cloud_msg_type_str(msg->header.type),
            cloud_msg_source_str(source));

    msg_category_t category = CLOUD_MSG_CATEGORY(msg->header.type);

    /* Handle OTA and App messages specially */
    if (category == MSG_CAT_OTA)
    {
        handle_ota_message(msg, source);
    }
    else if (category == MSG_CAT_APP)
    {
        handle_app_message(msg, source);
    }

    /* Call registered handlers */
    k_mutex_lock(&client.mutex, K_FOREVER);
    for (int i = 0; i < client.handler_count; i++)
    {
        if (client.handlers[i].category == category ||
            client.handlers[i].category == 0xFF)
        { /* Wildcard */
            int ret = client.handlers[i].handler(msg, source);
            if (ret != 0)
            {
                break; /* Handler consumed message */
            }
        }
    }
    k_mutex_unlock(&client.mutex);
}

static void handle_ota_message(const cloud_message_t *msg, msg_source_t source)
{
    switch (msg->header.type)
    {
    case MSG_TYPE_FW_AVAILABLE:
        LOG_INF("Firmware update available from %s", cloud_msg_source_str(source));
        break;

    case MSG_TYPE_FW_METADATA:
        if (msg->payload && msg->header.payload_len >= sizeof(payload_fw_metadata_t))
        {
            payload_fw_metadata_t *meta = (payload_fw_metadata_t *)msg->payload;
            LOG_INF("FW: v%d.%d.%d, size=%u, chunks=%u",
                    meta->version[0], meta->version[1], meta->version[2],
                    meta->size, meta->chunk_count);

            client.ota_transfer.active = true;
            client.ota_transfer.type = MSG_TYPE_FW_CHUNK;
            client.ota_transfer.source = source;
            client.ota_transfer.total_size = meta->size;
            client.ota_transfer.received = 0;
            client.ota_transfer.expected_chunks = meta->chunk_count;
            client.ota_transfer.received_chunks = 0;
            memcpy(client.ota_transfer.hash, meta->hash, 32);
        }
        break;

    case MSG_TYPE_FW_CHUNK:
        if (msg->payload && client.ota_transfer.active)
        {
            payload_chunk_t *chunk = (payload_chunk_t *)msg->payload;
            size_t data_len = msg->header.payload_len - offsetof(payload_chunk_t, data);

            LOG_DBG("FW chunk %u/%u, offset=%u, size=%zu",
                    chunk->chunk_index + 1, client.ota_transfer.expected_chunks,
                    chunk->offset, data_len);

            if (client.ota_handler)
            {
                client.ota_handler(chunk->data, data_len,
                                   chunk->offset, client.ota_transfer.total_size,
                                   source);
            }

            client.ota_transfer.received += data_len;
            client.ota_transfer.received_chunks++;
            client.stats.ota_chunks_rx++;

            /* Send ACK */
            cloud_message_t ack;
            cloud_msg_init(&ack.header, MSG_TYPE_FW_CHUNK_ACK, MSG_SOURCE_INTERNAL);
            ack.header.payload_len = sizeof(uint16_t);
            ack.payload = (uint8_t *)&chunk->chunk_index;
            cloud_client_send(&ack, source);
        }
        break;

    case MSG_TYPE_FW_COMPLETE:
        LOG_INF("Firmware transfer complete");
        client.ota_transfer.active = false;
        break;

    default:
        break;
    }
}

static void handle_app_message(const cloud_message_t *msg, msg_source_t source)
{
    switch (msg->header.type)
    {
    case MSG_TYPE_APP_AVAILABLE:
        LOG_INF("App update available from %s", cloud_msg_source_str(source));
        break;

    case MSG_TYPE_APP_METADATA:
        if (msg->payload && msg->header.payload_len >= sizeof(payload_app_metadata_t))
        {
            payload_app_metadata_t *meta = (payload_app_metadata_t *)msg->payload;
            LOG_INF("App: %s v%d.%d.%d, size=%u",
                    meta->name, meta->version[0], meta->version[1], meta->version[2],
                    meta->size);

            client.app_transfer.active = true;
            client.app_transfer.type = MSG_TYPE_APP_CHUNK;
            client.app_transfer.source = source;
            strncpy(client.app_transfer.app_id, meta->app_id, 31);
            client.app_transfer.total_size = meta->size;
            client.app_transfer.received = 0;
            client.app_transfer.expected_chunks = meta->chunk_count;
            client.app_transfer.received_chunks = 0;
            memcpy(client.app_transfer.hash, meta->hash, 32);
        }
        break;

    case MSG_TYPE_APP_CHUNK:
        if (msg->payload && client.app_transfer.active)
        {
            payload_chunk_t *chunk = (payload_chunk_t *)msg->payload;
            size_t data_len = msg->header.payload_len - offsetof(payload_chunk_t, data);

            LOG_DBG("App chunk %u/%u, offset=%u",
                    chunk->chunk_index + 1, client.app_transfer.expected_chunks,
                    chunk->offset);

            if (client.app_data_handler)
            {
                client.app_data_handler(client.app_transfer.app_id,
                                        chunk->data, data_len,
                                        chunk->offset, client.app_transfer.total_size,
                                        source);
            }

            client.app_transfer.received += data_len;
            client.app_transfer.received_chunks++;
            client.stats.app_chunks_rx++;

            /* Send ACK */
            cloud_message_t ack;
            cloud_msg_init(&ack.header, MSG_TYPE_APP_CHUNK_ACK, MSG_SOURCE_INTERNAL);
            ack.header.payload_len = sizeof(uint16_t);
            ack.payload = (uint8_t *)&chunk->chunk_index;
            cloud_client_send(&ack, source);
        }
        break;

    case MSG_TYPE_APP_COMPLETE:
        LOG_INF("App transfer complete: %s", client.app_transfer.app_id);
        if (client.app_complete_handler)
        {
            client.app_complete_handler(client.app_transfer.app_id,
                                        NULL, /* TODO: Pass metadata */
                                        true, source);
        }
        client.app_transfer.active = false;
        break;

    default:
        break;
    }
}

static void heartbeat_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    cloud_client_heartbeat();
}

static int send_via_transport(const uint8_t *data, size_t len, msg_source_t dest)
{
    int ret = -ENOTSUP;

    switch (dest)
    {
    case MSG_SOURCE_CLOUD:
        if (client.cloud_state >= CLOUD_STATE_CONNECTED)
        {
            /* TODO: Send via WebSocket client */
            /* ret = akira_ws_send_binary(data, len); */
            LOG_DBG("Send to cloud: %zu bytes", len);
            ret = 0; /* Stub */
        }
        break;

    case MSG_SOURCE_BT_APP:
        if (client.bt_state >= CLOUD_STATE_CONNECTED)
        {
            /* TODO: Send via Bluetooth */
            /* ret = bt_send_data(data, len); */
            LOG_DBG("Send to BT app: %zu bytes", len);
            ret = 0; /* Stub */
        }
        break;

    case MSG_SOURCE_WEB_SERVER:
        if (client.web_state >= CLOUD_STATE_CONNECTED)
        {
            /* TODO: Send via HTTP WebSocket */
            /* ret = akira_http_ws_send_binary(-1, data, len); */
            LOG_DBG("Send to web: %zu bytes", len);
            ret = 0; /* Stub */
        }
        break;

    default:
        LOG_WRN("Unknown destination: %d", dest);
        break;
    }

    return ret;
}
