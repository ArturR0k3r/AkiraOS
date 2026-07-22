/**
 * @file akira_mesh.h
 * @brief AkiraMesh Protocol - Hardware-Agnostic Mesh Networking
 *
 * Custom lightweight mesh protocol supporting:
 * - Multi-transport (BLE Mesh, 802.15.4, WiFi ESP-NOW)
 * - Multi-hop routing with AODV-based route discovery
 * - WASM app distribution across mesh
 * - State synchronization between nodes
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 * @stability experimental
 * @since 1.5
 */

#ifndef AKIRA_MESH_H
#define AKIRA_MESH_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>
#include "connectivity/radio_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum values */
#define AKIRA_MESH_MAX_NODES      CONFIG_AKIRA_MESH_MAX_NODES
#define AKIRA_MESH_MAX_HOPS       CONFIG_AKIRA_MESH_MAX_HOPS
#define AKIRA_MESH_NODE_ID_LEN    8
#define AKIRA_MESH_APP_NAME_LEN   32

/* Explicit transport selection. CC1121 and LR2021 both register as
 * RADIO_TYPE_SUBGHZ, so capability-mask matching can't tell them apart —
 * SUBGHZ/LORA are acquired by name ("CC1121"/"LR2021"), BLE by radio type. */
typedef enum {
    AKIRA_MESH_TRANSPORT_SUBGHZ = 0, /* CC1121 (FSK/OOK) */
    AKIRA_MESH_TRANSPORT_LORA,       /* LR2021 (LoRa) */
    AKIRA_MESH_TRANSPORT_BLE,        /* radio_ble.c */
} akira_mesh_transport_t;

/* Node role */
typedef enum {
    AKIRA_MESH_ROLE_NODE = 0,      /* Regular mesh node */
    AKIRA_MESH_ROLE_GATEWAY,       /* Gateway to external network */
    AKIRA_MESH_ROLE_PROVISIONER,   /* Can provision new nodes */
} akira_mesh_role_t;

/* Message types */
typedef enum {
    AKIRA_MESH_MSG_BEACON = 0,     /* Node discovery beacon */
    AKIRA_MESH_MSG_ROUTE_REQ,      /* Route request (AODV RREQ) */
    AKIRA_MESH_MSG_ROUTE_REPLY,    /* Route reply (AODV RREP) */
    AKIRA_MESH_MSG_ROUTE_ERROR,    /* Route error (AODV RERR) */
    AKIRA_MESH_MSG_DATA,           /* User data */
    AKIRA_MESH_MSG_ACK,            /* End-to-end delivery ACK */
    AKIRA_MESH_MSG_APP_CHUNK,      /* WASM app chunk */
    AKIRA_MESH_MSG_STATE_SYNC,     /* State synchronization */
    AKIRA_MESH_MSG_APP_START,      /* WASM app transfer header (name/size/count) */
    AKIRA_MESH_MSG_APP_STATUS_REQ, /* Query receiver's reassembly progress for an app_id */
    AKIRA_MESH_MSG_APP_STATUS_RESP,/* Reply: received-chunk bitmap, for resume */
} akira_mesh_msg_type_t;

/* Mesh configuration */
typedef struct {
    uint8_t node_id[AKIRA_MESH_NODE_ID_LEN];  /* Unique node ID */
    char node_name[AKIRA_MESH_APP_NAME_LEN];  /* Human-readable name */
    akira_mesh_role_t role;                   /* Node role */
    akira_mesh_transport_t transport;         /* Radio backend to use */
    uint8_t max_hops;                         /* Maximum hop count */
    uint32_t beacon_interval_ms;              /* Beacon transmission interval */
} akira_mesh_config_t;

/* Node information */
typedef struct {
    uint8_t node_id[AKIRA_MESH_NODE_ID_LEN];
    char name[AKIRA_MESH_APP_NAME_LEN];
    akira_mesh_role_t role;
    uint8_t hop_count;                        /* Hops from this node */
    int8_t rssi;                              /* Signal strength */
    uint8_t lqi;                              /* Link quality */
    uint32_t last_seen;                       /* Timestamp (ms) */
} akira_mesh_node_info_t;

/* Mesh statistics */
typedef struct {
    uint32_t nodes_discovered;
    uint32_t messages_sent;
    uint32_t messages_received;
    uint32_t messages_forwarded;
    uint32_t routes_active;
    uint32_t apps_distributed;
} akira_mesh_stats_t;

/* Mesh app-distribution RX (reassembly) progress */
typedef struct {
    bool     active;
    uint32_t app_id;
    char     app_name[AKIRA_MESH_APP_NAME_LEN];
    uint32_t total_len;
    uint16_t chunk_count;
    uint16_t received_count;
    uint32_t age_ms;          /* time since last accepted START/chunk */
} akira_mesh_app_rx_status_t;

/* Message reception callback */
typedef void (*akira_mesh_rx_cb_t)(const uint8_t *src_id, const uint8_t *data,
                                   size_t len, void *user_data);

/**
 * @brief Initialize AkiraMesh protocol
 *
 * @param config Mesh configuration
 * @return 0 on success, negative errno on failure
 */
int akira_mesh_init(const akira_mesh_config_t *config);

/**
 * @brief Start mesh networking
 *
 * Begins beacon transmission and node discovery.
 *
 * @return 0 on success, negative errno on failure
 */
int akira_mesh_start(void);

/**
 * @brief Stop mesh networking
 *
 * @return 0 on success, negative errno on failure
 */
int akira_mesh_stop(void);

/**
 * @brief Send data to specific node
 *
 * Performs route discovery if needed and sends data via mesh.
 *
 * @param dest_id Destination node ID
 * @param data Data to send
 * @param len Data length
 * @return 0 on success, negative errno on failure
 */
int akira_mesh_send(const uint8_t *dest_id, const uint8_t *data, size_t len);

/**
 * @brief Broadcast data to all nodes
 *
 * @param data Data to broadcast
 * @param len Data length
 * @param max_hops Maximum hop count
 * @return 0 on success, negative errno on failure
 */
int akira_mesh_broadcast(const uint8_t *data, size_t len, uint8_t max_hops);

/**
 * @brief Get list of discovered nodes
 *
 * @param nodes Array to store node information
 * @param max_nodes Maximum nodes to return
 * @return Number of nodes returned, or negative errno on failure
 */
int akira_mesh_get_nodes(akira_mesh_node_info_t *nodes, size_t max_nodes);

/**
 * @brief Get mesh statistics
 *
 * @param stats Pointer to statistics structure
 * @return 0 on success, negative errno on failure
 */
int akira_mesh_get_stats(akira_mesh_stats_t *stats);

/**
 * @brief Get current app-distribution reassembly progress (RX side)
 *
 * @param out Pointer to status structure
 * @return 0 on success, negative errno on failure
 */
int akira_mesh_get_app_rx_status(akira_mesh_app_rx_status_t *out);

/**
 * @brief Register receive callback
 *
 * @param callback Callback function
 * @param user_data User data passed to callback
 * @return 0 on success, negative errno on failure
 */
int akira_mesh_register_rx_callback(akira_mesh_rx_cb_t callback, void *user_data);

/**
 * @brief Distribute a WASM app to one mesh node
 *
 * Fragments the binary into MTU-sized chunks and sends them to @p dest_id
 * over the existing reliable unicast transport (per-chunk ACK + AODV route
 * discovery). Requires a radio that implements get_max_payload(); returns
 * -ENOSYS otherwise. Call once per destination node.
 *
 * @param dest_id Destination node ID
 * @param app_name Application name
 * @param app_data Application binary data
 * @param app_len Application length
 * @return 0 on success, negative errno on failure
 */
int akira_mesh_distribute_app(const uint8_t *dest_id, const char *app_name,
                              const uint8_t *app_data, size_t app_len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MESH_H */
