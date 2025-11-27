/**
 * @file cloud_protocol.c
 * @brief Cloud Protocol Implementation
 */

#include "cloud_protocol.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(cloud_proto, CONFIG_AKIRA_LOG_LEVEL);

static uint16_t sequence_counter = 0;

void cloud_msg_init(cloud_msg_header_t *hdr, msg_type_t type, msg_source_t source)
{
    if (!hdr)
    {
        return;
    }

    memset(hdr, 0, sizeof(*hdr));
    hdr->magic[0] = CLOUD_MSG_MAGIC_0;
    hdr->magic[1] = CLOUD_MSG_MAGIC_1;
    hdr->version = (AKIRA_PROTOCOL_VERSION_MAJOR << 4) | AKIRA_PROTOCOL_VERSION_MINOR;
    hdr->type = (uint8_t)type;
    hdr->source = (uint8_t)source;
    hdr->flags = MSG_FLAG_NONE;
    hdr->seq = sequence_counter++;
    hdr->payload_len = 0;
    hdr->timestamp = (uint32_t)(k_uptime_get() / 1000);
}

int cloud_msg_serialize(const cloud_message_t *msg, uint8_t *buffer, size_t buffer_len)
{
    if (!msg || !buffer)
    {
        return -EINVAL;
    }

    size_t total_size = CLOUD_MSG_HEADER_SIZE + msg->header.payload_len;
    if (buffer_len < total_size)
    {
        return -ENOMEM;
    }

    /* Copy header */
    memcpy(buffer, &msg->header, CLOUD_MSG_HEADER_SIZE);

    /* Copy payload if present */
    if (msg->payload && msg->header.payload_len > 0)
    {
        memcpy(buffer + CLOUD_MSG_HEADER_SIZE, msg->payload, msg->header.payload_len);
    }

    return (int)total_size;
}

int cloud_msg_parse(const uint8_t *buffer, size_t buffer_len, cloud_message_t *msg)
{
    if (!buffer || !msg || buffer_len < CLOUD_MSG_HEADER_SIZE)
    {
        return -EINVAL;
    }

    /* Copy header */
    memcpy(&msg->header, buffer, CLOUD_MSG_HEADER_SIZE);

    /* Validate magic */
    if (!CLOUD_MSG_IS_VALID(&msg->header))
    {
        LOG_ERR("Invalid message magic");
        return -EPROTO;
    }

    /* Check buffer has enough data */
    size_t total_size = CLOUD_MSG_HEADER_SIZE + msg->header.payload_len;
    if (buffer_len < total_size)
    {
        LOG_ERR("Buffer too small for payload");
        return -EMSGSIZE;
    }

    /* Allocate and copy payload */
    if (msg->header.payload_len > 0)
    {
        msg->payload = k_malloc(msg->header.payload_len);
        if (!msg->payload)
        {
            LOG_ERR("Failed to allocate payload");
            return -ENOMEM;
        }
        memcpy(msg->payload, buffer + CLOUD_MSG_HEADER_SIZE, msg->header.payload_len);
    }
    else
    {
        msg->payload = NULL;
    }

    return 0;
}

void cloud_msg_free(cloud_message_t *msg)
{
    if (msg && msg->payload)
    {
        k_free(msg->payload);
        msg->payload = NULL;
        msg->header.payload_len = 0;
    }
}

const char *cloud_msg_type_str(msg_type_t type)
{
    switch (type)
    {
    /* System */
    case MSG_TYPE_HEARTBEAT:
        return "HEARTBEAT";
    case MSG_TYPE_STATUS_REQUEST:
        return "STATUS_REQUEST";
    case MSG_TYPE_STATUS_RESPONSE:
        return "STATUS_RESPONSE";
    case MSG_TYPE_CONFIG_GET:
        return "CONFIG_GET";
    case MSG_TYPE_CONFIG_SET:
        return "CONFIG_SET";
    case MSG_TYPE_CONFIG_RESPONSE:
        return "CONFIG_RESPONSE";
    case MSG_TYPE_AUTH_REQUEST:
        return "AUTH_REQUEST";
    case MSG_TYPE_AUTH_RESPONSE:
        return "AUTH_RESPONSE";
    case MSG_TYPE_ERROR:
        return "ERROR";

    /* OTA */
    case MSG_TYPE_FW_CHECK:
        return "FW_CHECK";
    case MSG_TYPE_FW_AVAILABLE:
        return "FW_AVAILABLE";
    case MSG_TYPE_FW_REQUEST:
        return "FW_REQUEST";
    case MSG_TYPE_FW_METADATA:
        return "FW_METADATA";
    case MSG_TYPE_FW_CHUNK:
        return "FW_CHUNK";
    case MSG_TYPE_FW_CHUNK_ACK:
        return "FW_CHUNK_ACK";
    case MSG_TYPE_FW_COMPLETE:
        return "FW_COMPLETE";
    case MSG_TYPE_FW_VERIFY:
        return "FW_VERIFY";
    case MSG_TYPE_FW_APPLY:
        return "FW_APPLY";

    /* App */
    case MSG_TYPE_APP_LIST_REQUEST:
        return "APP_LIST_REQUEST";
    case MSG_TYPE_APP_LIST_RESPONSE:
        return "APP_LIST_RESPONSE";
    case MSG_TYPE_APP_CHECK:
        return "APP_CHECK";
    case MSG_TYPE_APP_AVAILABLE:
        return "APP_AVAILABLE";
    case MSG_TYPE_APP_REQUEST:
        return "APP_REQUEST";
    case MSG_TYPE_APP_METADATA:
        return "APP_METADATA";
    case MSG_TYPE_APP_CHUNK:
        return "APP_CHUNK";
    case MSG_TYPE_APP_CHUNK_ACK:
        return "APP_CHUNK_ACK";
    case MSG_TYPE_APP_COMPLETE:
        return "APP_COMPLETE";
    case MSG_TYPE_APP_INSTALL:
        return "APP_INSTALL";
    case MSG_TYPE_APP_UNINSTALL:
        return "APP_UNINSTALL";
    case MSG_TYPE_APP_START:
        return "APP_START";
    case MSG_TYPE_APP_STOP:
        return "APP_STOP";

    /* Data */
    case MSG_TYPE_DATA_SYNC:
        return "DATA_SYNC";
    case MSG_TYPE_DATA_FETCH:
        return "DATA_FETCH";
    case MSG_TYPE_DATA_RESPONSE:
        return "DATA_RESPONSE";
    case MSG_TYPE_SENSOR_DATA:
        return "SENSOR_DATA";
    case MSG_TYPE_LOG_DATA:
        return "LOG_DATA";

    /* Control */
    case MSG_TYPE_CMD_REBOOT:
        return "CMD_REBOOT";
    case MSG_TYPE_CMD_FACTORY_RESET:
        return "CMD_FACTORY_RESET";
    case MSG_TYPE_CMD_SLEEP:
        return "CMD_SLEEP";
    case MSG_TYPE_CMD_WAKE:
        return "CMD_WAKE";
    case MSG_TYPE_CMD_CUSTOM:
        return "CMD_CUSTOM";

    /* Notify */
    case MSG_TYPE_NOTIFY_PUSH:
        return "NOTIFY_PUSH";
    case MSG_TYPE_NOTIFY_ALERT:
        return "NOTIFY_ALERT";
    case MSG_TYPE_NOTIFY_ACK:
        return "NOTIFY_ACK";

    default:
        return "UNKNOWN";
    }
}

const char *cloud_msg_source_str(msg_source_t source)
{
    switch (source)
    {
    case MSG_SOURCE_CLOUD:
        return "CLOUD";
    case MSG_SOURCE_BT_APP:
        return "BT_APP";
    case MSG_SOURCE_WEB_SERVER:
        return "WEB_SERVER";
    case MSG_SOURCE_USB:
        return "USB";
    case MSG_SOURCE_INTERNAL:
        return "INTERNAL";
    default:
        return "UNKNOWN";
    }
}
