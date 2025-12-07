/**
 * @file cloud_protocol.h
 * @brief AkiraOS Cloud Communication Protocol
 *
 * Defines the message protocol for communication between AkiraOS and:
 * - Remote cloud servers (AkiraHub)
 * - AkiraApp (Bluetooth mobile app)
 * - Local web server
 *
 * All sources use the same message format for consistency.
 */

#ifndef AKIRA_CLOUD_PROTOCOL_H
#define AKIRA_CLOUD_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*===========================================================================*/
    /* Protocol Version                                                          */
    /*===========================================================================*/

#define AKIRA_PROTOCOL_VERSION_MAJOR 1
#define AKIRA_PROTOCOL_VERSION_MINOR 0

    /*===========================================================================*/
    /* Message Sources                                                           */
    /*===========================================================================*/

    /** Where the message originated from */
    typedef enum
    {
        MSG_SOURCE_UNKNOWN = 0x00,
        MSG_SOURCE_CLOUD = 0x01,      /**< Remote server (WebSocket/CoAP/MQTT) */
        MSG_SOURCE_BT_APP = 0x02,     /**< AkiraApp via Bluetooth */
        MSG_SOURCE_WEB_SERVER = 0x03, /**< Local web server */
        MSG_SOURCE_USB = 0x04,        /**< USB connection */
        MSG_SOURCE_INTERNAL = 0x05,   /**< Internal system message */
    } msg_source_t;

    /*===========================================================================*/
    /* Message Categories                                                        */
    /*===========================================================================*/

    /** High-level message categories */
    typedef enum
    {
        MSG_CAT_SYSTEM = 0x00,  /**< System messages (status, config) */
        MSG_CAT_OTA = 0x10,     /**< Firmware updates */
        MSG_CAT_APP = 0x20,     /**< WASM app management */
        MSG_CAT_DATA = 0x30,    /**< Data sync/storage */
        MSG_CAT_CONTROL = 0x40, /**< Remote control commands */
        MSG_CAT_NOTIFY = 0x50,  /**< Notifications */
    } msg_category_t;

    /*===========================================================================*/
    /* Message Types                                                             */
    /*===========================================================================*/

    /** Specific message types */
    typedef enum
    {
        /* System Messages (0x00-0x0F) */
        MSG_TYPE_HEARTBEAT = 0x00,       /**< Keep-alive ping/pong */
        MSG_TYPE_STATUS_REQUEST = 0x01,  /**< Request device status */
        MSG_TYPE_STATUS_RESPONSE = 0x02, /**< Device status response */
        MSG_TYPE_CONFIG_GET = 0x03,      /**< Get configuration */
        MSG_TYPE_CONFIG_SET = 0x04,      /**< Set configuration */
        MSG_TYPE_CONFIG_RESPONSE = 0x05, /**< Configuration response */
        MSG_TYPE_AUTH_REQUEST = 0x06,    /**< Authentication request */
        MSG_TYPE_AUTH_RESPONSE = 0x07,   /**< Authentication response */
        MSG_TYPE_ERROR = 0x0F,           /**< Error message */

        /* OTA Messages (0x10-0x1F) */
        MSG_TYPE_FW_CHECK = 0x10,     /**< Check for firmware updates */
        MSG_TYPE_FW_AVAILABLE = 0x11, /**< Firmware update available */
        MSG_TYPE_FW_REQUEST = 0x12,   /**< Request firmware download */
        MSG_TYPE_FW_METADATA = 0x13,  /**< Firmware metadata (size, hash) */
        MSG_TYPE_FW_CHUNK = 0x14,     /**< Firmware data chunk */
        MSG_TYPE_FW_CHUNK_ACK = 0x15, /**< Chunk acknowledgment */
        MSG_TYPE_FW_COMPLETE = 0x16,  /**< Transfer complete */
        MSG_TYPE_FW_VERIFY = 0x17,    /**< Verification result */
        MSG_TYPE_FW_APPLY = 0x18,     /**< Apply update (reboot) */

        /* App Messages (0x20-0x2F) */
        MSG_TYPE_APP_LIST_REQUEST = 0x20,  /**< Request app catalog */
        MSG_TYPE_APP_LIST_RESPONSE = 0x21, /**< App catalog response */
        MSG_TYPE_APP_CHECK = 0x22,         /**< Check for app updates */
        MSG_TYPE_APP_AVAILABLE = 0x23,     /**< App/update available */
        MSG_TYPE_APP_REQUEST = 0x24,       /**< Request app download */
        MSG_TYPE_APP_METADATA = 0x25,      /**< App metadata (name, size, perms) */
        MSG_TYPE_APP_CHUNK = 0x26,         /**< App binary chunk */
        MSG_TYPE_APP_CHUNK_ACK = 0x27,     /**< Chunk acknowledgment */
        MSG_TYPE_APP_COMPLETE = 0x28,      /**< Transfer complete */
        MSG_TYPE_APP_INSTALL = 0x29,       /**< Install app */
        MSG_TYPE_APP_UNINSTALL = 0x2A,     /**< Uninstall app */
        MSG_TYPE_APP_START = 0x2B,         /**< Start app */
        MSG_TYPE_APP_STOP = 0x2C,          /**< Stop app */

        /* Data Messages (0x30-0x3F) */
        MSG_TYPE_DATA_SYNC = 0x30,     /**< Sync data to cloud */
        MSG_TYPE_DATA_FETCH = 0x31,    /**< Fetch data from cloud */
        MSG_TYPE_DATA_RESPONSE = 0x32, /**< Data response */
        MSG_TYPE_SENSOR_DATA = 0x33,   /**< Sensor readings */
        MSG_TYPE_LOG_DATA = 0x34,      /**< Log/telemetry data */

        /* Control Messages (0x40-0x4F) */
        MSG_TYPE_CMD_REBOOT = 0x40,        /**< Reboot device */
        MSG_TYPE_CMD_FACTORY_RESET = 0x41, /**< Factory reset */
        MSG_TYPE_CMD_SLEEP = 0x42,         /**< Enter sleep mode */
        MSG_TYPE_CMD_WAKE = 0x43,          /**< Wake from sleep */
        MSG_TYPE_CMD_CUSTOM = 0x4F,        /**< Custom command */

        /* Notification Messages (0x50-0x5F) */
        MSG_TYPE_NOTIFY_PUSH = 0x50,  /**< Push notification */
        MSG_TYPE_NOTIFY_ALERT = 0x51, /**< Alert (high priority) */
        MSG_TYPE_NOTIFY_ACK = 0x52,   /**< Notification acknowledged */

    } msg_type_t;

    /*===========================================================================*/
    /* Message Header                                                            */
    /*===========================================================================*/

    /** Message header (always first in message) */
    typedef struct __attribute__((packed))
    {
        uint8_t magic[2];     /**< Magic bytes: 'A', 'K' */
        uint8_t version;      /**< Protocol version */
        uint8_t type;         /**< Message type (msg_type_t) */
        uint8_t source;       /**< Message source (msg_source_t) */
        uint8_t flags;        /**< Message flags */
        uint16_t seq;         /**< Sequence number */
        uint32_t payload_len; /**< Payload length */
        uint32_t timestamp;   /**< Unix timestamp (optional) */
    } cloud_msg_header_t;

#define CLOUD_MSG_MAGIC_0 'A'
#define CLOUD_MSG_MAGIC_1 'K'

/** Message flags */
#define MSG_FLAG_NONE 0x00
#define MSG_FLAG_RESPONSE 0x01   /**< This is a response */
#define MSG_FLAG_ENCRYPTED 0x02  /**< Payload is encrypted */
#define MSG_FLAG_COMPRESSED 0x04 /**< Payload is compressed */
#define MSG_FLAG_NEEDS_ACK 0x08  /**< Requires acknowledgment */
#define MSG_FLAG_FINAL 0x10      /**< Final chunk in sequence */
#define MSG_FLAG_ERROR 0x80      /**< Error flag */

    /*===========================================================================*/
    /* Payload Structures                                                        */
    /*===========================================================================*/

    /** Device status payload */
    typedef struct __attribute__((packed))
    {
        uint8_t fw_version[4]; /**< Major, Minor, Patch, Build */
        uint32_t uptime_sec;   /**< Uptime in seconds */
        uint16_t battery_mv;   /**< Battery voltage in mV */
        uint8_t battery_pct;   /**< Battery percentage */
        uint8_t cpu_usage;     /**< CPU usage percentage */
        uint32_t free_memory;  /**< Free heap memory */
        uint32_t free_storage; /**< Free storage */
        uint8_t app_count;     /**< Number of installed apps */
        uint8_t running_apps;  /**< Number of running apps */
    } payload_status_t;

    /** Firmware metadata payload */
    typedef struct __attribute__((packed))
    {
        uint8_t version[4];      /**< Major, Minor, Patch, Build */
        uint32_t size;           /**< Total firmware size */
        uint8_t hash[32];        /**< SHA-256 hash */
        uint16_t chunk_size;     /**< Chunk size for transfer */
        uint16_t chunk_count;    /**< Total number of chunks */
        char release_notes[128]; /**< Release notes (optional) */
    } payload_fw_metadata_t;

    /** Firmware/App chunk payload */
    typedef struct __attribute__((packed))
    {
        uint16_t chunk_index; /**< Chunk index (0-based) */
        uint16_t chunk_size;  /**< This chunk's size */
        uint32_t offset;      /**< Offset in file */
        uint8_t data[];       /**< Chunk data (variable) */
    } payload_chunk_t;

    /** App metadata payload */
    typedef struct __attribute__((packed))
    {
        char app_id[32];      /**< Unique app identifier */
        char name[32];        /**< Display name */
        uint8_t version[4];   /**< App version */
        uint32_t size;        /**< WASM binary size */
        uint8_t hash[32];     /**< SHA-256 hash */
        uint64_t permissions; /**< Required permissions bitmap */
        uint16_t chunk_size;  /**< Chunk size for transfer */
        uint16_t chunk_count; /**< Total chunks */
    } payload_app_metadata_t;

    /** App list entry */
    typedef struct __attribute__((packed))
    {
        char app_id[32];    /**< App identifier */
        char name[32];      /**< Display name */
        uint8_t version[4]; /**< Version */
        uint8_t installed;  /**< Is installed? */
        uint8_t has_update; /**< Update available? */
    } payload_app_entry_t;

    /** Notification payload */
    typedef struct __attribute__((packed))
    {
        uint8_t priority;   /**< 0=low, 1=normal, 2=high */
        uint8_t category;   /**< Notification category */
        uint16_t title_len; /**< Title length */
        uint16_t body_len;  /**< Body length */
        char data[];        /**< Title + Body (variable) */
    } payload_notification_t;

    /** Error payload */
    typedef struct __attribute__((packed))
    {
        uint16_t error_code; /**< Error code */
        uint8_t severity;    /**< 0=warn, 1=error, 2=fatal */
        char message[128];   /**< Error message */
    } payload_error_t;

    /*===========================================================================*/
    /* Complete Message Structure                                                */
    /*===========================================================================*/

    /** Complete cloud message */
    typedef struct
    {
        cloud_msg_header_t header;
        uint8_t *payload; /**< Dynamically allocated payload */
    } cloud_message_t;

    /*===========================================================================*/
    /* Helper Macros                                                             */
    /*===========================================================================*/

#define CLOUD_MSG_HEADER_SIZE sizeof(cloud_msg_header_t)
#define CLOUD_MSG_MAX_SIZE (CLOUD_MSG_HEADER_SIZE + 65536)

/** Check if message header is valid */
#define CLOUD_MSG_IS_VALID(hdr)              \
    ((hdr)->magic[0] == CLOUD_MSG_MAGIC_0 && \
     (hdr)->magic[1] == CLOUD_MSG_MAGIC_1)

/** Get message category from type */
#define CLOUD_MSG_CATEGORY(type) ((type) & 0xF0)

/** Check if message is a response */
#define CLOUD_MSG_IS_RESPONSE(hdr) (((hdr)->flags & MSG_FLAG_RESPONSE) != 0)

    /*===========================================================================*/
    /* Protocol Functions                                                        */
    /*===========================================================================*/

    /**
     * @brief Initialize a message header
     * @param hdr Header to initialize
     * @param type Message type
     * @param source Message source
     */
    void cloud_msg_init(cloud_msg_header_t *hdr, msg_type_t type, msg_source_t source);

    /**
     * @brief Serialize message to buffer
     * @param msg Message to serialize
     * @param buffer Output buffer
     * @param buffer_len Buffer length
     * @return Bytes written or negative error
     */
    int cloud_msg_serialize(const cloud_message_t *msg, uint8_t *buffer, size_t buffer_len);

    /**
     * @brief Parse message from buffer
     * @param buffer Input buffer
     * @param buffer_len Buffer length
     * @param msg Output message (payload allocated)
     * @return 0 on success
     */
    int cloud_msg_parse(const uint8_t *buffer, size_t buffer_len, cloud_message_t *msg);

    /**
     * @brief Free message payload
     * @param msg Message to free
     */
    void cloud_msg_free(cloud_message_t *msg);

    /**
     * @brief Get string name for message type
     * @param type Message type
     * @return Type name string
     */
    const char *cloud_msg_type_str(msg_type_t type);

    /**
     * @brief Get string name for message source
     * @param source Message source
     * @return Source name string
     */
    const char *cloud_msg_source_str(msg_source_t source);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CLOUD_PROTOCOL_H */
