#ifndef AKIRA_MESH_MAC_H
#define AKIRA_MESH_MAC_H

#include <stdint.h>
#include <stddef.h>
#include "connectivity/radio_interface.h"

#define MESH_MAC_PACKET_BUF_SIZE 256

typedef enum {
    MESH_MAC_PRIO_CRITICAL = 0,  /* RREQ/RREP/RERR — routing/topology */
    MESH_MAC_PRIO_HIGH,          /* ACK — per-packet reliability */
    MESH_MAC_PRIO_MEDIUM,        /* STATUS_QUERY/STATUS_RESP/STREAM_STATUS_* */
    MESH_MAC_PRIO_LOW,           /* DATA/APP_CHUNK/STREAM_DATA */
} mesh_mac_prio_t;

/* rssi: signal strength of this specific reception in dBm (see
 * radio_get_last_rx_rssi), or 0 on radios/conditions where it's unavailable —
 * callers that care (BEACON, for the discovered-node list) should treat 0 as
 * "unknown" rather than a real reading. */
typedef void (*mesh_mac_rx_cb_t)(const uint8_t *buf, size_t len, int16_t rssi, void *ctx);

/* Takes ownership of radio (already ops->init()'d by the caller — MAC only
 * drives send/recv/get_rssi, it doesn't bring the hardware up or down). */
int mesh_mac_init(radio_handle_t *radio);
int mesh_mac_deinit(void);

/* Enqueue for transmission. Non-blocking — copies into the priority lane's
 * queue and returns immediately; the dedicated MAC TX thread does the actual
 * (possibly slow, e.g. ~240ms over BLE) radio send. -EBUSY if that lane is
 * full, -ENODEV if mesh_mac_init hasn't been called. */
int mesh_mac_send(mesh_mac_prio_t prio, const uint8_t *buf, size_t len);

/* Maps an AKIRA_MESH_MSG_* wire type to its TX lane — for relay/retransmit
 * paths that forward an already-built frame and don't know its priority
 * from context the way a direct sender does. */
mesh_mac_prio_t mesh_mac_prio_for_msg_type(uint8_t msg_type);

/* Exactly one callback at a time (matches akira_mesh_register_rx_callback's
 * existing single-callback contract) — called from the MAC RX thread. */
int mesh_mac_register_rx_cb(mesh_mac_rx_cb_t cb, void *ctx);

#endif /* AKIRA_MESH_MAC_H */
