/**
 * @file mesh_manager.c
 * @brief AkiraMesh Protocol Manager Implementation
 *
 * Implements lightweight mesh networking with multi-transport support.
 * Foundation implementation - full routing and state sync to be completed.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
*/

#include "connectivity/akira_mesh.h"
#include "connectivity/radio_interface.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_mesh, CONFIG_AKIRA_LOG_LEVEL);

/* Mesh protocol header */
struct __packed mesh_header {
    uint8_t version;                         /* Protocol version */
    uint8_t msg_type;                        /* Message type */
    uint8_t ttl;                             /* Time to live (hops) */
    uint8_t src_id[AKIRA_MESH_NODE_ID_LEN]; /* Source node ID */
    uint8_t dest_id[AKIRA_MESH_NODE_ID_LEN];/* Destination node ID */
    uint16_t seq_num;                        /* Sequence number */
};

/* Mesh manager state */
static struct {
    akira_mesh_config_t config;
    akira_mesh_stats_t stats;
    radio_handle_t *radio;
    akira_mesh_rx_cb_t rx_callback;
    void *rx_user_data;
    akira_mesh_node_info_t nodes[AKIRA_MESH_MAX_NODES];
    uint8_t node_count;
    uint16_t seq_num;
    struct k_work_delayable beacon_work;
    bool initialized;
    bool started;
} mesh_state;

/* Forward declarations */
static void mesh_beacon_work_handler(struct k_work *work);
static void mesh_radio_event_handler(const radio_event_t *event, void *user_data);

/* Work queue for mesh operations */
K_WORK_DELAYABLE_DEFINE(beacon_work, mesh_beacon_work_handler);

int akira_mesh_init(const akira_mesh_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }
    
    if (mesh_state.initialized) {
        LOG_WRN("AkiraMesh already initialized");
        return -EALREADY;
    }
    
    /* Copy configuration */
    memcpy(&mesh_state.config, config, sizeof(akira_mesh_config_t));
    memset(&mesh_state.stats, 0, sizeof(akira_mesh_stats_t));
    mesh_state.node_count = 0;
    mesh_state.seq_num = 0;
    
    /* Select radio transport */
    switch (config->transport) {
    case AKIRA_MESH_TRANSPORT_BLE:
        mesh_state.radio = radio_manager_get(RADIO_TYPE_BLE);
        if (!mesh_state.radio) {
            LOG_ERR("BLE radio not available for mesh");
            return -ENODEV;
        }
        LOG_INF("AkiraMesh using BLE transport");
        break;
        
    case AKIRA_MESH_TRANSPORT_802154:
        mesh_state.radio = radio_manager_get(RADIO_TYPE_802154);
        if (!mesh_state.radio) {
            LOG_ERR("802.15.4 radio not available for mesh");
            return -ENODEV;
        }
        LOG_INF("AkiraMesh using 802.15.4 transport");
        break;
        
    case AKIRA_MESH_TRANSPORT_ESPNOW:
        mesh_state.radio = radio_manager_get(RADIO_TYPE_WIFI);
        if (!mesh_state.radio) {
            LOG_ERR("WiFi radio not available for ESP-NOW mesh");
            return -ENODEV;
        }
        LOG_INF("AkiraMesh using ESP-NOW transport");
        break;
        
    default:
        LOG_ERR("Invalid mesh transport: %d", config->transport);
        return -EINVAL;
    }
    
    /* Register event callback with radio */
    if (mesh_state.radio->ops->set_event_callback) {
        mesh_state.radio->ops->set_event_callback(mesh_state.radio,
                                                 mesh_radio_event_handler,
                                                 NULL);
    }
    
    mesh_state.initialized = true;
    
    LOG_INF("AkiraMesh initialized");
    LOG_INF("  Node ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
            config->node_id[0], config->node_id[1], config->node_id[2], config->node_id[3],
            config->node_id[4], config->node_id[5], config->node_id[6], config->node_id[7]);
    LOG_INF("  Name: %s, Role: %d, Max hops: %d",
            config->node_name, config->role, config->max_hops);
    
    return 0;
}

static void mesh_beacon_work_handler(struct k_work *work)
{
    if (!mesh_state.started) {
        return;
    }
    
    /* Build beacon message */
    uint8_t beacon[64];
    struct mesh_header *hdr = (struct mesh_header *)beacon;
    
    hdr->version = 1;
    hdr->msg_type = AKIRA_MESH_MSG_BEACON;
    hdr->ttl = 1;  /* Beacons are single-hop */
    memcpy(hdr->src_id, mesh_state.config.node_id, AKIRA_MESH_NODE_ID_LEN);
    memset(hdr->dest_id, 0xFF, AKIRA_MESH_NODE_ID_LEN);  /* Broadcast */
    hdr->seq_num = mesh_state.seq_num++;
    
    /* Add node name to beacon payload */
    size_t name_len = strlen(mesh_state.config.node_name);
    memcpy(beacon + sizeof(struct mesh_header), mesh_state.config.node_name, name_len);
    
    size_t total_len = sizeof(struct mesh_header) + name_len;
    
    /* Send beacon via radio */
    int ret = radio_send(mesh_state.radio, beacon, total_len);
    if (ret < 0) {
        LOG_WRN("Failed to send mesh beacon: %d", ret);
    } else {
        mesh_state.stats.messages_sent++;
        LOG_DBG("Sent mesh beacon #%d", hdr->seq_num);
    }
    
    /* Reschedule beacon (every 10 seconds) */
    k_work_schedule(&beacon_work, K_SECONDS(10));
}

static void mesh_radio_event_handler(const radio_event_t *event, void *user_data)
{
    if (event->type != RADIO_EVENT_RX_DONE) {
        return;
    }
    
    /* Parse received mesh packet */
    if (event->len < sizeof(struct mesh_header)) {
        return;  /* Too small */
    }
    
    const struct mesh_header *hdr = (const struct mesh_header *)event->data;
    
    /* Check protocol version */
    if (hdr->version != 1) {
        return;  /* Unknown version */
    }
    
    mesh_state.stats.messages_received++;
    
    /* Handle different message types */
    switch (hdr->msg_type) {
    case AKIRA_MESH_MSG_BEACON:
        LOG_DBG("Received beacon from node %02x:%02x:...",
                hdr->src_id[0], hdr->src_id[1]);
        
        /* Update node table */
        bool found = false;
        for (uint8_t i = 0; i < mesh_state.node_count; i++) {
            if (memcmp(mesh_state.nodes[i].node_id, hdr->src_id, 
                      AKIRA_MESH_NODE_ID_LEN) == 0) {
                /* Update existing node */
                mesh_state.nodes[i].last_seen = k_uptime_get_32();
                mesh_state.nodes[i].rssi = event->rssi;
                mesh_state.nodes[i].lqi = event->lqi;
                found = true;
                break;
            }
        }
        
        if (!found && mesh_state.node_count < AKIRA_MESH_MAX_NODES) {
            /* Add new node */
            akira_mesh_node_info_t *node = &mesh_state.nodes[mesh_state.node_count++];
            memcpy(node->node_id, hdr->src_id, AKIRA_MESH_NODE_ID_LEN);
            
            /* Extract node name from payload */
            size_t name_len = event->len - sizeof(struct mesh_header);
            if (name_len > 0 && name_len < sizeof(node->name)) {
                memcpy(node->name, event->data + sizeof(struct mesh_header), name_len);
                node->name[name_len] = '\0';
            }
            
            node->hop_count = 1;
            node->rssi = event->rssi;
            node->lqi = event->lqi;
            node->last_seen = k_uptime_get_32();
            node->role = AKIRA_MESH_ROLE_NODE;
            
            mesh_state.stats.nodes_discovered++;
            LOG_INF("Discovered new mesh node: %s (RSSI: %d dBm)",
                    node->name, event->rssi);
        }
        break;
        
    case AKIRA_MESH_MSG_DATA:
        /* Forward to application callback */
        if (mesh_state.rx_callback) {
            const uint8_t *payload = event->data + sizeof(struct mesh_header);
            size_t payload_len = event->len - sizeof(struct mesh_header);
            mesh_state.rx_callback(hdr->src_id, payload, payload_len,
                                  mesh_state.rx_user_data);
        }
        break;
        
    default:
        LOG_DBG("Unhandled mesh message type: %d", hdr->msg_type);
        break;
    }
}

int akira_mesh_start(void)
{
    if (!mesh_state.initialized) {
        return -ENODEV;
    }
    
    if (mesh_state.started) {
        LOG_WRN("AkiraMesh already started");
        return 0;
    }
    
    LOG_INF("Starting AkiraMesh");
    
    /* Start beacon transmission */
    k_work_schedule(&beacon_work, K_SECONDS(1));
    
    mesh_state.started = true;
    
    LOG_INF("AkiraMesh started - broadcasting beacons");
    return 0;
}

int akira_mesh_stop(void)
{
    if (!mesh_state.started) {
        return 0;
    }
    
    LOG_INF("Stopping AkiraMesh");
    
    /* Cancel beacon work */
    k_work_cancel_delayable(&beacon_work);
    
    mesh_state.started = false;
    
    return 0;
}

int akira_mesh_send(const uint8_t *dest_id, const uint8_t *data, size_t len)
{
    if (!mesh_state.initialized || !dest_id || !data) {
        return -EINVAL;
    }
    
    if (!mesh_state.started) {
        return -ENODEV;
    }
    
    /* Build mesh packet */
    uint8_t packet[256];
    struct mesh_header *hdr = (struct mesh_header *)packet;
    
    if (len > sizeof(packet) - sizeof(struct mesh_header)) {
        return -EMSGSIZE;
    }
    
    hdr->version = 1;
    hdr->msg_type = AKIRA_MESH_MSG_DATA;
    hdr->ttl = mesh_state.config.max_hops;
    memcpy(hdr->src_id, mesh_state.config.node_id, AKIRA_MESH_NODE_ID_LEN);
    memcpy(hdr->dest_id, dest_id, AKIRA_MESH_NODE_ID_LEN);
    hdr->seq_num = mesh_state.seq_num++;
    
    memcpy(packet + sizeof(struct mesh_header), data, len);
    
    int ret = radio_send(mesh_state.radio, packet, sizeof(struct mesh_header) + len);
    if (ret < 0) {
        LOG_ERR("Failed to send mesh packet: %d", ret);
        return ret;
    }
    
    mesh_state.stats.messages_sent++;
    return 0;
}

int akira_mesh_broadcast(const uint8_t *data, size_t len, uint8_t max_hops)
{
    uint8_t broadcast_id[AKIRA_MESH_NODE_ID_LEN];
    memset(broadcast_id, 0xFF, sizeof(broadcast_id));
    
    return akira_mesh_send(broadcast_id, data, len);
}

int akira_mesh_get_nodes(akira_mesh_node_info_t *nodes, size_t max_nodes)
{
    if (!nodes || max_nodes == 0) {
        return -EINVAL;
    }
    
    if (!mesh_state.initialized) {
        return -ENODEV;
    }
    
    size_t copy_count = MIN(mesh_state.node_count, max_nodes);
    memcpy(nodes, mesh_state.nodes, copy_count * sizeof(akira_mesh_node_info_t));
    
    return copy_count;
}

int akira_mesh_get_stats(akira_mesh_stats_t *stats)
{
    if (!stats) {
        return -EINVAL;
    }
    
    if (!mesh_state.initialized) {
        return -ENODEV;
    }
    
    memcpy(stats, &mesh_state.stats, sizeof(akira_mesh_stats_t));
    return 0;
}

int akira_mesh_register_rx_callback(akira_mesh_rx_cb_t callback, void *user_data)
{
    mesh_state.rx_callback = callback;
    mesh_state.rx_user_data = user_data;
    LOG_DBG("Mesh RX callback registered");
    return 0;
}

int akira_mesh_distribute_app(const char *app_name, const uint8_t *app_data, size_t app_len)
{
    if (!app_name || !app_data || app_len == 0) {
        return -EINVAL;
    }
    
    if (!mesh_state.initialized || !mesh_state.started) {
        return -ENODEV;
    }
    
    LOG_INF("Distributing WASM app '%s' (%zu bytes) across mesh", app_name, app_len);
    
    /* Chunk app into mesh-sized packets and broadcast */
    /* This is a placeholder - full implementation would handle chunking,
     * reassembly, and acknowledgments */
    
    mesh_state.stats.apps_distributed++;
    
    return 0;
}
