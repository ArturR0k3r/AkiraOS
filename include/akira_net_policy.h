/**
 * @file akira_net_policy.h
 * @brief Per-app network egress policy
 *
 * A policy provider is asked before a WASM app opens a connection, binds or
 * listens on a port, or makes the firmware fetch a URL on its behalf (OTA).
 * Any provider can refuse; with no provider every request is allowed (the
 * network.use capability still applies).
 *
 * The policy an app declares in its manifest ("network_policy") reaches
 * providers through the admission LOAD context (akira_admission.h), so a
 * provider can cache it per app without the core keeping a copy.
 *
 * @code
 * static int acme_net(const struct akira_net_policy_ctx *ctx, void *user)
 * {
 *     return strcmp(ctx->host, "api.acme.com") == 0 ? 0 : -EACCES;
 * }
 * AKIRA_NET_POLICY_DEFINE(acme_net_policy, acme_net, NULL);
 * @endcode
 *
 * Providers run on the calling app's thread and must not block on I/O.
 *
 * @stability experimental
 * @since 1.7
 */

#ifndef AKIRA_NET_POLICY_H
#define AKIRA_NET_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/iterable_sections.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AKIRA_NET_POLICY_MAX_HOSTS 4
#define AKIRA_NET_POLICY_MAX_PORTS 8
#define AKIRA_NET_POLICY_HOST_LEN  64

/**
 * The "network_policy" object of an app manifest, as parsed. allowed_hosts
 * entries may be "*" (any host); an allowed_ports entry of 0 means any port.
 */
struct akira_net_policy_decl {
    bool present;  /**< "network_policy" was in the manifest */
    bool deny_all; /**< { "deny_all": true } */
    uint8_t host_count;
    uint8_t port_count;
    char allowed_hosts[AKIRA_NET_POLICY_MAX_HOSTS][AKIRA_NET_POLICY_HOST_LEN];
    uint16_t allowed_ports[AKIRA_NET_POLICY_MAX_PORTS];
};

/** What the app is trying to do. */
enum akira_net_op {
    AKIRA_NET_OP_CONNECT = 0, /**< outbound TCP/UDP connect: host, port */
    AKIRA_NET_OP_BIND,        /**< bind a local port: port (host NULL) */
    AKIRA_NET_OP_LISTEN,      /**< listen on a bound socket (host NULL, port 0) */
    AKIRA_NET_OP_FETCH_URL,   /**< firmware fetches a URL for the app: host, port */
};

/** One request. Strings are valid only during the callback. */
struct akira_net_policy_ctx {
    enum akira_net_op op;
    const char *app_name; /**< "" if the caller could not be identified */
    const char *host;     /**< hostname or address literal, or NULL */
    uint16_t port;        /**< 0 if not applicable */
};

/** Return 0 to allow, a negative errno (normally -EACCES) to refuse. */
typedef int (*akira_net_policy_fn)(const struct akira_net_policy_ctx *ctx,
                                   void *user_data);

/** One registered provider. Create with AKIRA_NET_POLICY_DEFINE(). */
struct akira_net_policy {
    akira_net_policy_fn fn;
    void *user_data;
};

/** Register a network policy provider. */
#define AKIRA_NET_POLICY_DEFINE(id, fn, user)                                      \
    const STRUCT_SECTION_ITERABLE(akira_net_policy, id) = { (fn), (user) }

/**
 * @brief Ask every provider. Called by AkiraOS network APIs.
 *
 * Logs and audits a refusal. @return 0 if allowed, negative errno if not.
 */
int akira_net_policy_check(const struct akira_net_policy_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_NET_POLICY_H */
