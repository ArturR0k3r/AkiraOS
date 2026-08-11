#ifndef AKIRA_MESH_TRANSPORT_H
#define AKIRA_MESH_TRANSPORT_H

#include "connectivity/akira_mesh.h"

void mesh_transport_module_init(const akira_mesh_config_t *config,
                                akira_mesh_stats_t *stats, size_t mtu,
                                akira_mesh_rx_cb_t *rx_cb_ptr, void **rx_ctx_ptr);

int mesh_transport_send_reliable(const uint8_t *dest_id, uint8_t msg_type,
                                 const uint8_t *data, size_t len);

/* Entry point for DATA/ACK/STREAM_* frames from the generic dispatch. */
void mesh_transport_handle_frame(const uint8_t *buf, size_t len);

void mesh_transport_tick(uint32_t now_ms);

/* Called once per real end-to-end ack or timed-out give-up, after Transport's
 * own ack_table bookkeeping — lets mesh_app_dist.c's blocking send wait for
 * "its" ack without Transport knowing app-distribution's wait structs exist. */
typedef void (*mesh_transport_ack_notify_t)(uint16_t seq, const uint8_t *src_id, bool timed_out);
void mesh_transport_register_ack_notify(mesh_transport_ack_notify_t cb);

#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
/* Install an E2E session derived from a RREQ/RREP handshake — called by
 * mesh_aodv.c after ECDH completes. Narrow, deliberate exception to
 * downward-only layering: session key material is a Security-layer concern
 * that both Network (derives it during route discovery) and Transport (uses
 * it to encrypt DATA) touch; Transport owns the actual storage. */
void mesh_transport_install_session(const uint8_t *local_id, const uint8_t *peer_id,
                                    const uint8_t *shared_secret, size_t shared_len,
                                    uint32_t now_ms, uint32_t ttl_ms);

/* Extend peer_id's session expiry by CONFIG_AKIRA_MESH_SESSION_LIFETIME_S
 * from now — called on every accepted stream-mode frame during an active
 * transfer so a long transfer on a slow link doesn't outlive the session's
 * original TTL and fail decryption at reassembly for no cryptographic
 * reason. No-op if no session exists yet for peer_id. */
void mesh_transport_touch_session(const uint8_t *peer_id, uint32_t now_ms);

/* Re-attempts any DATA send that was queued because no session existed yet
 * for peer_id (mesh_transport_send_reliable's cold path stashes it instead
 * of sending broken unencrypted bytes) — called by
 * mesh_router_derive_and_install_session() right after it installs the
 * session, so the retry lands the instant both route and key exist. No-op
 * if nothing is queued for peer_id. */
void mesh_transport_retry_pending(const uint8_t *peer_id);
#endif

#endif /* AKIRA_MESH_TRANSPORT_H */
