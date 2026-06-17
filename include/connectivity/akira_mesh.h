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

/* Transport selection is purely capability-based — the mesh acquires the first
 * free radio whose capabilities match the configured mask. */
#define AKIRA_MESH_CAPS_SUBGHZ (RADIO_CAP_TX | RADIO_CAP_RX | RADIO_CAP_BAND_SUBGHZ)
#define AKIRA_MESH_CAPS_802154 (RADIO_CAP_TX | RADIO_CAP_RX | RADIO_CAP_CSMA_CA)
#define AKIRA_MESH_CAPS_BLE    (RADIO_CAP_TX | RADIO_CAP_RX | RADIO_CAP_MOD_BLE_PHY)

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
} akira_mesh_msg_type_t;

/* Mesh configuration */
typedef struct {
    uint8_t node_id[AKIRA_MESH_NODE_ID_LEN];  /* Unique node ID */
    char node_name[AKIRA_MESH_APP_NAME_LEN];  /* Human-readable name */
    akira_mesh_role_t role;                   /* Node role */
    uint32_t transport_caps;                  /* Caps mask for radio acquisition */
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
 * @brief Register receive callback
 *
 * @param callback Callback function
 * @param user_data User data passed to callback
 * @return 0 on success, negative errno on failure
 */
int akira_mesh_register_rx_callback(akira_mesh_rx_cb_t callback, void *user_data);

/**
 * @brief Distribute WASM app across mesh
 *
 * Chunks app file and distributes to all nodes in mesh.
 *
 * @param app_name Application name
 * @param app_data Application binary data
 * @param app_len Application length
 * @return 0 on success, negative errno on failure
 */
int akira_mesh_distribute_app(const char *app_name, const uint8_t *app_data, size_t app_len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MESH_H */
