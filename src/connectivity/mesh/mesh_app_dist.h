#ifndef AKIRA_MESH_APP_DIST_H
#define AKIRA_MESH_APP_DIST_H

#include "connectivity/akira_mesh.h"

void mesh_app_dist_module_init(const akira_mesh_config_t *config, akira_mesh_stats_t *stats,
                               size_t mtu, akira_mesh_rx_cb_t *rx_cb_ptr, void **rx_ctx_ptr);
void mesh_app_dist_handle_frame(const uint8_t *buf, size_t len);
int  akira_mesh_get_app_rx_status(akira_mesh_app_rx_status_t *out);
int  akira_mesh_distribute_app(const uint8_t *dest_id, const char *app_name,
                               const uint8_t *app_data, size_t app_len);

#endif /* AKIRA_MESH_APP_DIST_H */
