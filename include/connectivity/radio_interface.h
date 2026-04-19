/**
 * @file radio_interface.h
 * @brief Hardware-agnostic Radio Abstraction Layer (RAL) for AkiraOS
 *
 * Provides unified interface for WiFi, BLE, and 802.15.4 radios to enable
 * protocol stacks (Matter, Thread, AkiraMesh) to work across all hardware platforms.
 *
 * Design Philosophy:
 * - Hardware agnostic: Protocols don't know what radio they're using
 * - Capability discovery: Runtime detection of radio features
 * - Zero-copy where possible: Direct buffer management
 * - Thread-safe: Concurrent protocol access to different radios
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
*/

#ifndef AKIRA_RADIO_INTERFACE_H
#define AKIRA_RADIO_INTERFACE_H

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Radio type identifiers */
typedef enum {
    RADIO_TYPE_NONE = 0,
    RADIO_TYPE_WIFI,       /* IEEE 802.11 WiFi (2.4GHz/5GHz) */
    RADIO_TYPE_BLE,        /* Bluetooth Low Energy 5.x */
    RADIO_TYPE_802154,     /* IEEE 802.15.4 (Thread, Zigbee) */
    RADIO_TYPE_LORA,       /* LoRaWAN (future) */
    RADIO_TYPE_MAX
} radio_type_t;

/* Radio capability flags */
#define RADIO_CAP_TX            BIT(0)  /* Transmit capability */
#define RADIO_CAP_RX            BIT(1)  /* Receive capability */
#define RADIO_CAP_SCAN          BIT(2)  /* Network scanning */
#define RADIO_CAP_MESH          BIT(3)  /* Mesh networking support */
#define RADIO_CAP_ENCRYPTION    BIT(4)  /* Hardware encryption */
#define RADIO_CAP_PROMISCUOUS   BIT(5)  /* Promiscuous mode */
#define RADIO_CAP_LOW_POWER     BIT(6)  /* Low power sleep modes */
#define RADIO_CAP_MULTICAST     BIT(7)  /* Multicast/broadcast */
#define RADIO_CAP_CCA           BIT(8)  /* Clear Channel Assessment */
#define RADIO_CAP_AUTO_ACK      BIT(9)  /* Automatic acknowledgments */
#define RADIO_CAP_CSMA_CA       BIT(10) /* CSMA/CA collision avoidance */
#define RADIO_CAP_RAW_MODE      BIT(11) /* Raw frame access */

/* Radio states */
typedef enum {
    RADIO_STATE_OFF = 0,      /* Radio powered off */
    RADIO_STATE_IDLE,         /* Powered on, not active */
    RADIO_STATE_RX,           /* Receiving */
    RADIO_STATE_TX,           /* Transmitting */
    RADIO_STATE_SCAN,         /* Scanning for networks */
    RADIO_STATE_SLEEP,        /* Low power sleep */
    RADIO_STATE_ERROR         /* Error state */
} radio_state_t;

/* Radio configuration parameters */
typedef struct {
    uint16_t channel;         /* Channel number/frequency */
    int8_t tx_power;          /* TX power in dBm */
    uint16_t mtu;             /* Maximum transmission unit */
    uint8_t retry_count;      /* Automatic retry count */
    uint32_t timeout_ms;      /* Operation timeout */
    bool promiscuous;         /* Promiscuous mode enable */
    bool auto_ack;            /* Auto acknowledgment enable */
} radio_config_t;

/* Radio statistics */
typedef struct {
    uint64_t tx_packets;      /* Transmitted packets */
    uint64_t rx_packets;      /* Received packets */
    uint64_t tx_bytes;        /* Transmitted bytes */
    uint64_t rx_bytes;        /* Received bytes */
    uint32_t tx_errors;       /* Transmission errors */
    uint32_t rx_errors;       /* Reception errors */
    uint32_t tx_dropped;      /* Dropped TX packets */
    uint32_t rx_dropped;      /* Dropped RX packets */
    int8_t rssi;              /* Current RSSI (dBm) */
    uint8_t lqi;              /* Link Quality Indicator (0-255) */
    uint32_t noise_floor;     /* Noise floor (dBm) */
} radio_stats_t;

/* Radio event types */
typedef enum {
    RADIO_EVENT_RX_DONE,      /* Packet received */
    RADIO_EVENT_TX_DONE,      /* Packet transmitted */
    RADIO_EVENT_TX_FAILED,    /* Transmission failed */
    RADIO_EVENT_SCAN_DONE,    /* Scan completed */
    RADIO_EVENT_CONNECTED,    /* Connected to network */
    RADIO_EVENT_DISCONNECTED, /* Disconnected from network */
    RADIO_EVENT_ERROR,        /* Hardware error */
    RADIO_EVENT_CCA_BUSY,     /* Channel busy (CSMA/CA) */
} radio_event_type_t;

/* Radio event structure */
typedef struct {
    radio_event_type_t type;  /* Event type */
    const uint8_t *data;      /* Event data (packet, scan result, etc.) */
    size_t len;               /* Data length */
    int8_t rssi;              /* RSSI for RX events */
    uint8_t lqi;              /* LQI for RX events */
    void *user_data;          /* User context */
} radio_event_t;

/* Forward declaration */
struct radio_handle;

/* Radio event callback */
typedef void (*radio_event_cb_t)(const radio_event_t *event, void *user_data);

/* Radio operations interface (vtable pattern) */
typedef struct {
    /* Initialize radio hardware */
    int (*init)(struct radio_handle *handle);
    
    /* Deinitialize radio hardware */
    int (*deinit)(struct radio_handle *handle);
    
    /* Configure radio parameters */
    int (*configure)(struct radio_handle *handle, const radio_config_t *config);
    
    /* Get current configuration */
    int (*get_config)(struct radio_handle *handle, radio_config_t *config);
    
    /* Transmit packet */
    int (*send)(struct radio_handle *handle, const uint8_t *data, size_t len);
    
    /* Receive packet (synchronous) */
    int (*recv)(struct radio_handle *handle, uint8_t *buf, size_t buf_len, uint32_t timeout_ms);
    
    /* Start network scan */
    int (*scan)(struct radio_handle *handle, uint32_t timeout_ms);
    
    /* Set radio state */
    int (*set_state)(struct radio_handle *handle, radio_state_t state);
    
    /* Get radio state */
    radio_state_t (*get_state)(struct radio_handle *handle);
    
    /* Get radio statistics */
    int (*get_stats)(struct radio_handle *handle, radio_stats_t *stats);
    
    /* Reset radio */
    int (*reset)(struct radio_handle *handle);
    
    /* Set event callback */
    int (*set_event_callback)(struct radio_handle *handle, radio_event_cb_t callback, void *user_data);
    
    /* Get hardware address (MAC/EUI-64) */
    int (*get_hw_addr)(struct radio_handle *handle, uint8_t *addr, size_t *addr_len);
} radio_ops_t;

/* Radio handle structure */
typedef struct radio_handle {
    radio_type_t type;             /* Radio type */
    const char *name;              /* Human-readable name */
    uint32_t capabilities;         /* Capability flags */
    const radio_ops_t *ops;        /* Operations vtable */
    struct net_if *net_if;         /* Associated network interface (optional) */
    void *priv_data;               /* Private driver data */
    struct k_mutex lock;           /* Handle lock for thread safety */
    radio_state_t state;           /* Current state */
    radio_event_cb_t event_cb;     /* Event callback */
    void *event_user_data;         /* Event callback user data */
} radio_handle_t;

/* Radio manager functions */

/**
 * @brief Initialize the radio manager subsystem
 * 
 * Must be called before any other radio functions. Discovers and initializes
 * all available radio hardware.
 * 
 * @return 0 on success, negative errno on failure
 */
int radio_manager_init(void);

/**
 * @brief Register a radio with the manager
 * 
 * Called by platform-specific radio drivers to register their hardware.
 * 
 * @param handle Initialized radio handle
 * @return 0 on success, negative errno on failure
 */
int radio_manager_register(radio_handle_t *handle);

/**
 * @brief Unregister a radio from the manager
 * 
 * @param handle Radio handle to unregister
 * @return 0 on success, negative errno on failure
 */
int radio_manager_unregister(radio_handle_t *handle);

/**
 * @brief Get a radio handle by type
 * 
 * Retrieves the first available radio of the specified type. Use
 * radio_manager_get_all() to enumerate multiple radios of same type.
 * 
 * @param type Radio type to get
 * @return Radio handle or NULL if not available
 */
radio_handle_t *radio_manager_get(radio_type_t type);

/**
 * @brief Get all radios of a specific type
 * 
 * @param type Radio type to query (RADIO_TYPE_NONE for all radios)
 * @param handles Array to store radio handles
 * @param max_handles Maximum number of handles to return
 * @return Number of handles returned, or negative errno on failure
 */
int radio_manager_get_all(radio_type_t type, radio_handle_t **handles, size_t max_handles);

/**
 * @brief Check if a radio type is available
 * 
 * @param type Radio type to check
 * @return true if available, false otherwise
 */
bool radio_manager_is_available(radio_type_t type);

/**
 * @brief Get radio capabilities
 * 
 * @param handle Radio handle
 * @return Capability flags, or 0 if handle is NULL
 */
uint32_t radio_get_capabilities(const radio_handle_t *handle);

/**
 * @brief Check if radio has specific capability
 * 
 * @param handle Radio handle
 * @param capability Capability flag to check
 * @return true if capability is supported
 */
bool radio_has_capability(const radio_handle_t *handle, uint32_t capability);

/**
 * @brief Get human-readable name for radio type
 * 
 * @param type Radio type
 * @return String name or "Unknown"
 */
const char *radio_type_to_string(radio_type_t type);

/**
 * @brief Get human-readable name for radio state
 * 
 * @param state Radio state
 * @return String name or "Unknown"
 */
const char *radio_state_to_string(radio_state_t state);

/* Convenience wrappers for common operations */

/**
 * @brief Send data using specified radio
 * 
 * Thread-safe wrapper with automatic locking.
 * 
 * @param handle Radio handle
 * @param data Data to send
 * @param len Data length
 * @return Number of bytes sent, or negative errno on failure
 */
static inline int radio_send(radio_handle_t *handle, const uint8_t *data, size_t len)
{
    if (!handle || !handle->ops || !handle->ops->send) {
        return -ENOTSUP;
    }
    
    k_mutex_lock(&handle->lock, K_FOREVER);
    int ret = handle->ops->send(handle, data, len);
    k_mutex_unlock(&handle->lock);
    
    return ret;
}

/**
 * @brief Receive data from specified radio
 * 
 * Thread-safe wrapper with automatic locking.
 * 
 * @param handle Radio handle
 * @param buf Buffer to receive into
 * @param buf_len Buffer length
 * @param timeout_ms Timeout in milliseconds (or K_FOREVER)
 * @return Number of bytes received, or negative errno on failure
 */
static inline int radio_recv(radio_handle_t *handle, uint8_t *buf, size_t buf_len, uint32_t timeout_ms)
{
    if (!handle || !handle->ops || !handle->ops->recv) {
        return -ENOTSUP;
    }
    
    k_mutex_lock(&handle->lock, K_FOREVER);
    int ret = handle->ops->recv(handle, buf, buf_len, timeout_ms);
    k_mutex_unlock(&handle->lock);
    
    return ret;
}

/**
 * @brief Configure radio parameters
 * 
 * @param handle Radio handle
 * @param config Configuration parameters
 * @return 0 on success, negative errno on failure
 */
static inline int radio_configure(radio_handle_t *handle, const radio_config_t *config)
{
    if (!handle || !handle->ops || !handle->ops->configure) {
        return -ENOTSUP;
    }
    
    k_mutex_lock(&handle->lock, K_FOREVER);
    int ret = handle->ops->configure(handle, config);
    k_mutex_unlock(&handle->lock);
    
    return ret;
}

/**
 * @brief Get radio statistics
 * 
 * @param handle Radio handle
 * @param stats Pointer to statistics structure
 * @return 0 on success, negative errno on failure
 */
static inline int radio_get_stats(radio_handle_t *handle, radio_stats_t *stats)
{
    if (!handle || !handle->ops || !handle->ops->get_stats) {
        return -ENOTSUP;
    }
    
    k_mutex_lock(&handle->lock, K_FOREVER);
    int ret = handle->ops->get_stats(handle, stats);
    k_mutex_unlock(&handle->lock);
    
    return ret;
}

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_RADIO_INTERFACE_H */
