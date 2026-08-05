/**
 * @file akira_sync.h
 * @brief AkiraSync — leaderless distributed ordering and fair timestamps.
 *
 * AkiraSync gives a small group of AkiraOS devices a single agreed-upon order
 * of events and a single agreed-upon clock, with no server, no leader and no
 * vote messages.
 *
 * The mechanism is a DAG of gossiped events ("gossip about gossip"): every
 * event names its creator's previous event (self-parent) and the peer event
 * that triggered it (other-parent). Causality is therefore recorded by the
 * graph's shape rather than by exchanging votes, so the bandwidth cost is just
 * the cost of gossip. Each node also records when it first saw every event;
 * the median of those first-sight times across the peer set becomes the
 * event's canonical timestamp. That yields a shared timeline without SNTP, a
 * GPS pulse, or any other external time source.
 *
 * Two tiers:
 *   - Tier A (default): hashes only, no signatures. Deterministic fair
 *     ordering that tolerates crashed and slow peers.
 *   - Tier B (CONFIG_AKIRA_SYNC_BFT): signed events, tolerating up to
 *     f = (n-1)/3 actively malicious peers.
 *
 * This layer is transport-agnostic by construction — see struct
 * akira_sync_transport. It has no dependency on any radio, link layer, board
 * or chip.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_SYNC_H
#define AKIRA_SYNC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Length of a node identifier, in bytes. */
#define AKIRA_SYNC_ID_LEN       8
/** Length of a truncated event hash, in bytes. */
#define AKIRA_SYNC_HASH_LEN     8
/** Maximum application payload carried by one event. */
#define AKIRA_SYNC_MAX_PAYLOAD  192
/** Maximum peers in one session (including self). */
#define AKIRA_SYNC_MAX_PEERS    CONFIG_AKIRA_SYNC_MAX_PEERS
/** Maximum length of a session identifier, including NUL. */
#define AKIRA_SYNC_SESSION_LEN  16

/* -------------------------------------------------------------------------
 * Transport binding
 * ------------------------------------------------------------------------- */

/**
 * @brief Called by a transport when a sync frame arrives from a peer.
 *
 * May run in the transport's own RX thread. Must not block.
 *
 * @param src_id  Sender node id (AKIRA_SYNC_ID_LEN bytes), or NULL if unknown.
 * @param buf     Frame bytes.
 * @param len     Frame length.
 * @param user_data Opaque pointer supplied to set_rx_cb().
 */
typedef void (*akira_sync_rx_cb_t)(const uint8_t *src_id, const void *buf,
                                   size_t len, void *user_data);

/**
 * @brief A way to move sync frames between peers.
 *
 * Deliberately minimal: sync only ever needs to flood a frame to the peer
 * group and be told when one arrives. Anything that can do those two things —
 * a radio mesh, a UDP socket, an in-process loopback used by tests — is a
 * valid transport. Unreliable and reordering transports are fine; the DAG
 * recovers from loss by gap detection and re-gossip.
 */
struct akira_sync_transport {
    /** Human-readable name, for logs. */
    const char *name;

    /** Flood a frame to every peer. Returns 0 or a negative errno. */
    int (*broadcast)(const void *buf, size_t len);

    /**
     * Install (or, with cb == NULL, remove) the receive callback.
     * Returns 0 or a negative errno.
     */
    int (*set_rx_cb)(akira_sync_rx_cb_t cb, void *user_data);

    /** Largest frame this transport will carry, in bytes. */
    size_t mtu;
};

/**
 * @brief Make @p transport the one AkiraSync uses.
 *
 * Must be called before akira_sync_open(). Passing NULL unbinds. A transport
 * whose mtu cannot hold a minimal event is rejected with -EMSGSIZE.
 *
 * @return 0 on success, negative errno on failure.
 */
int akira_sync_transport_register(const struct akira_sync_transport *transport);

/** @brief The currently bound transport, or NULL if none. */
const struct akira_sync_transport *akira_sync_transport_active(void);

/**
 * @brief Bind the built-in loopback transport.
 *
 * Delivers every broadcast straight back to this node. Always available, on
 * every target. Makes a single-node session behave exactly like a multi-node
 * one, which is what lets the ordering core be tested without any network.
 *
 * @return 0 on success, negative errno on failure.
 */
int akira_sync_transport_bind_loopback(void);

#if defined(CONFIG_AKIRA_SYNC_TRANSPORT_MESH)
/**
 * @brief Bind AkiraMesh as the transport.
 *
 * Only compiled when AkiraMesh is present. The mesh must already be
 * initialized and started.
 *
 * @return 0 on success, negative errno on failure.
 */
int akira_sync_transport_bind_mesh(void);
#endif

/* -------------------------------------------------------------------------
 * Session
 * ------------------------------------------------------------------------- */

/** Session configuration. */
typedef struct {
    /** Session name. All peers in one session must use the same string. */
    char     session_id[AKIRA_SYNC_SESSION_LEN];
    /** This node's identifier. Must be unique within the session. */
    uint8_t  self_id[AKIRA_SYNC_ID_LEN];
    /**
     * Interval at which an empty event is created when the app has nothing
     * to say, keeping the DAG advancing so pending events can finalize.
     * 0 selects CONFIG_AKIRA_SYNC_HEARTBEAT_MS.
     */
    uint32_t heartbeat_ms;
} akira_sync_config_t;

/** A finalized event, delivered in consensus order. */
typedef struct {
    /** Node that created the event. */
    uint8_t  creator[AKIRA_SYNC_ID_LEN];
    /** Fair timestamp: median of peers' first-sight times, microseconds. */
    uint64_t time_us;
    /** Position in the global total order. Dense and gap-free from 0. */
    uint32_t order;
    /** Payload length, in bytes. */
    uint16_t len;
    /** Payload as submitted by the creating node. */
    uint8_t  payload[AKIRA_SYNC_MAX_PAYLOAD];
} akira_sync_event_t;

/** Peer as seen by the local node. */
typedef struct {
    uint8_t  id[AKIRA_SYNC_ID_LEN];
    /** Highest self_seq seen from this peer. */
    uint32_t last_seq;
    /** Local uptime (ms) when this peer was last heard from. */
    uint32_t last_seen_ms;
    /** True once the peer has been evicted for falling too far behind. */
    bool     fallen_behind;
} akira_sync_peer_t;

/** Session counters. */
typedef struct {
    uint32_t events_created;
    uint32_t events_received;
    uint32_t events_finalized;
    uint32_t events_dropped;    /**< DAG ring overrun or malformed frames. */
    uint32_t rounds_decided;
    uint32_t peers_known;
} akira_sync_stats_t;

/**
 * @brief Join (or create) a session.
 *
 * A transport must be bound first. Peers are discovered from the events they
 * gossip; there is no join handshake and no membership authority.
 *
 * @return 0 on success, negative errno on failure.
 */
int akira_sync_open(const akira_sync_config_t *config);

/** @brief Leave the session and release its resources. @return 0 or errno. */
int akira_sync_close(void);

/**
 * @brief Submit a payload for ordering.
 *
 * Returns as soon as the payload is attached to a local event. The payload is
 * not yet ordered at that point — it surfaces later, in consensus position,
 * via akira_sync_event_pop().
 *
 * @return 0 on success, negative errno on failure.
 */
int akira_sync_submit(const void *payload, size_t len);

/**
 * @brief Pop the next finalized event, in consensus order.
 *
 * Every node in the session pops exactly the same events, in exactly the same
 * order, with exactly the same timestamps.
 *
 * @param out         Receives the event.
 * @param timeout_ms  0 to poll; otherwise block up to this long.
 * @return 0 on success, -EAGAIN if nothing is ready, negative errno on error.
 */
int akira_sync_event_pop(akira_sync_event_t *out, uint32_t timeout_ms);

/**
 * @brief Current fair network time, in microseconds.
 *
 * Agreed across the session without any external time source. Before enough
 * events have finalized to establish the offset, this falls back to local
 * uptime and reports -EAGAIN.
 *
 * @return 0 on success, -EAGAIN while still converging, negative errno on error.
 */
int akira_sync_time_now_us(uint64_t *out);

/**
 * @brief Copy the known-peer table.
 * @return Number of peers written, or negative errno.
 */
int akira_sync_peers(akira_sync_peer_t *out, size_t max_peers);

/** @brief Read session counters. @return 0 or negative errno. */
int akira_sync_get_stats(akira_sync_stats_t *out);

/** @brief True while a session is open. */
bool akira_sync_is_open(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SYNC_H */
