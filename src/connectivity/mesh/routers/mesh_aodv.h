#ifndef AKIRA_MESH_AODV_H
#define AKIRA_MESH_AODV_H

#include "connectivity/akira_mesh.h"

/* Registers "aodv" with mesh_router.h and stores the config/stats this
 * implementation needs. Call once, before the first mesh_router_acquire("aodv").
 * Returns -EIO if the long-term identity keypair can't be loaded/generated
 * (only possible when CONFIG_AKIRA_MESH_E2E_CRYPTO=y). */
int mesh_aodv_module_init(const akira_mesh_config_t *config, akira_mesh_stats_t *stats);

/* Neighbor-table snapshot — a named exception to "only talk through
 * mesh_router_ops_t": neighbor discovery is fundamentally part of a concrete
 * routing implementation, not something the generic vtable needs to expose,
 * but akira_mesh_get_nodes() needs a way to read it. */
int mesh_aodv_get_nodes(akira_mesh_node_info_t *nodes, size_t max_nodes);

/* Beacon frames are protocol-agnostic in framing but AODV-specific in what
 * they're used for (neighbor-table population) — mesh_manager.c's generic
 * dispatch routes AKIRA_MESH_MSG_BEACON here directly. */
void mesh_aodv_dispatch_beacon(const uint8_t *src_id, const uint8_t *payload, size_t plen,
                               int16_t rssi);

#endif /* AKIRA_MESH_AODV_H */
