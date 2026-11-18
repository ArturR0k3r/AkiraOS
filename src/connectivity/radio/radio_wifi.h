/**
 * @file radio_wifi.h
 * @brief WiFi RAL backend — mesh-mode setup, outside the generic radio_ops_t
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 */

#ifndef AKIRA_RADIO_WIFI_H
#define AKIRA_RADIO_WIFI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Select AP (gateway) vs. STA bring-up for the next wifi_radio_init().
 *
 * Mirrors bt_manager_set_mode()'s role: mesh_manager.c calls this before
 * acquiring/initializing the WiFi radio so wifi_radio_init() knows whether
 * to start the mesh SoftAP or STA-connect to it. Static config, set from the
 * node's role (AKIRA_MESH_ROLE_GATEWAY) — no runtime election.
 */
void radio_wifi_set_gateway(bool is_gateway);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_RADIO_WIFI_H */
