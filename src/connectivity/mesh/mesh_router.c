/**
 * @file mesh_router.c
 * @brief Routing-protocol registry — vtable + register/unregister/acquire/release,
 * mirroring radio_interface.h's radio_ops_t / radio_manager.c pattern so
 * routing algorithms are swappable instead of hardcoded to AODV.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 */

#include "mesh_router.h"
#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
#include "mesh_transport.h"
#endif
#include <string.h>
#include <errno.h>

#define MESH_ROUTER_MAX_REGISTERED 4

struct router_slot {
    char name[MESH_ROUTER_NAME_MAX];
    const mesh_router_ops_t *ops;
    bool used;
};

static struct router_slot s_routers[MESH_ROUTER_MAX_REGISTERED];
static const mesh_router_ops_t *s_active;
static char s_active_name[MESH_ROUTER_NAME_MAX];

int mesh_router_register(const char *name, const mesh_router_ops_t *ops)
{
    if (!name || !ops) return -EINVAL;
    for (int i = 0; i < MESH_ROUTER_MAX_REGISTERED; i++) {
        if (s_routers[i].used && strncmp(s_routers[i].name, name, MESH_ROUTER_NAME_MAX) == 0) {
            return -EALREADY;
        }
    }
    for (int i = 0; i < MESH_ROUTER_MAX_REGISTERED; i++) {
        if (!s_routers[i].used) {
            s_routers[i].used = true;
            strncpy(s_routers[i].name, name, MESH_ROUTER_NAME_MAX - 1);
            s_routers[i].name[MESH_ROUTER_NAME_MAX - 1] = '\0';
            s_routers[i].ops = ops;
            return 0;
        }
    }
    return -ENOMEM;
}

int mesh_router_unregister(const char *name)
{
    if (!name) return -EINVAL;
    if (s_active && strncmp(s_active_name, name, MESH_ROUTER_NAME_MAX) == 0) {
        return -EBUSY; /* release it first */
    }
    for (int i = 0; i < MESH_ROUTER_MAX_REGISTERED; i++) {
        if (s_routers[i].used && strncmp(s_routers[i].name, name, MESH_ROUTER_NAME_MAX) == 0) {
            s_routers[i].used = false;
            return 0;
        }
    }
    return -ENOENT;
}

int mesh_router_acquire(const char *name)
{
    if (!name) return -EINVAL;
    const mesh_router_ops_t *found = NULL;
    for (int i = 0; i < MESH_ROUTER_MAX_REGISTERED; i++) {
        if (s_routers[i].used && strncmp(s_routers[i].name, name, MESH_ROUTER_NAME_MAX) == 0) {
            found = s_routers[i].ops;
            break;
        }
    }
    if (!found) return -ENOENT;
    if (s_active) mesh_router_release();
    int ret = found->start ? found->start() : 0;
    if (ret) return ret;
    s_active = found;
    strncpy(s_active_name, name, MESH_ROUTER_NAME_MAX - 1);
    s_active_name[MESH_ROUTER_NAME_MAX - 1] = '\0';
    return 0;
}

int mesh_router_release(void)
{
    if (!s_active) return 0;
    if (s_active->stop) s_active->stop();
    s_active = NULL;
    s_active_name[0] = '\0';
    return 0;
}

const mesh_router_ops_t *mesh_router_get_active(void)
{
    return s_active;
}

#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
int mesh_router_derive_and_install_session(const uint8_t *my_priv,
                                           const uint8_t *local_id,
                                           const uint8_t *peer_pub,
                                           const uint8_t *peer_id,
                                           uint32_t now_ms, uint32_t lifetime_ms)
{
    uint8_t shared[MESH_CRYPTO_SHARED_LEN];
    int ret = mesh_crypto_p256_ecdh(my_priv, peer_pub, shared);
    if (ret == 0) {
        mesh_transport_install_session(local_id, peer_id, shared, sizeof(shared),
                                       now_ms, lifetime_ms);
        mesh_transport_retry_pending(peer_id);
    }
    memset(shared, 0, sizeof(shared));
    return ret;
}
#endif
