/**
 * @file ota_transport.c
 * @brief OTA Transport Registry Implementation
 */

#include "ota_transport.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ota_transport, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#define MAX_OTA_TRANSPORTS 4

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct {
    const ota_transport_ops_t *transports[MAX_OTA_TRANSPORTS];
    int count;
    
    ota_data_cb_t data_cb;
    ota_transport_complete_cb_t complete_cb;
    ota_transport_progress_cb_t progress_cb;
    
    struct k_mutex mutex;
} transport_registry;

static bool initialized = false;

/*===========================================================================*/
/* Internal Functions                                                        */
/*===========================================================================*/

static void init_registry(void)
{
    if (initialized) {
        return;
    }
    
    k_mutex_init(&transport_registry.mutex);
    memset(transport_registry.transports, 0, sizeof(transport_registry.transports));
    transport_registry.count = 0;
    initialized = true;
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int ota_transport_register(const ota_transport_ops_t *ops)
{
    if (!ops || !ops->name) {
        return -EINVAL;
    }
    
    init_registry();
    
    k_mutex_lock(&transport_registry.mutex, K_FOREVER);
    
    /* Check for duplicate */
    for (int i = 0; i < transport_registry.count; i++) {
        if (transport_registry.transports[i]->source == ops->source) {
            k_mutex_unlock(&transport_registry.mutex);
            LOG_WRN("Transport %s already registered", ops->name);
            return -EEXIST;
        }
    }
    
    if (transport_registry.count >= MAX_OTA_TRANSPORTS) {
        k_mutex_unlock(&transport_registry.mutex);
        LOG_ERR("Max OTA transports reached");
        return -ENOMEM;
    }
    
    transport_registry.transports[transport_registry.count++] = ops;
    
    k_mutex_unlock(&transport_registry.mutex);
    
    LOG_INF("Registered OTA transport: %s", ops->name);
    return 0;
}

int ota_transport_unregister(ota_source_t source)
{
    if (!initialized) {
        return -EINVAL;
    }
    
    k_mutex_lock(&transport_registry.mutex, K_FOREVER);
    
    for (int i = 0; i < transport_registry.count; i++) {
        if (transport_registry.transports[i]->source == source) {
            /* Shift remaining */
            for (int j = i; j < transport_registry.count - 1; j++) {
                transport_registry.transports[j] = transport_registry.transports[j + 1];
            }
            transport_registry.count--;
            k_mutex_unlock(&transport_registry.mutex);
            return 0;
        }
    }
    
    k_mutex_unlock(&transport_registry.mutex);
    return -ENOENT;
}

const ota_transport_ops_t *ota_transport_get(ota_source_t source)
{
    if (!initialized) {
        return NULL;
    }
    
    for (int i = 0; i < transport_registry.count; i++) {
        if (transport_registry.transports[i]->source == source) {
            return transport_registry.transports[i];
        }
    }
    
    return NULL;
}

void ota_transport_set_data_cb(ota_data_cb_t callback)
{
    transport_registry.data_cb = callback;
}

void ota_transport_set_complete_cb(ota_transport_complete_cb_t callback)
{
    transport_registry.complete_cb = callback;
}

void ota_transport_set_progress_cb(ota_transport_progress_cb_t callback)
{
    transport_registry.progress_cb = callback;
}

ota_source_t ota_transport_get_available(void)
{
    if (!initialized) {
        return OTA_SOURCE_NONE;
    }
    
    ota_source_t available = OTA_SOURCE_NONE;
    
    for (int i = 0; i < transport_registry.count; i++) {
        const ota_transport_ops_t *t = transport_registry.transports[i];
        if (t && t->is_available && t->is_available()) {
            available |= t->source;
        }
    }
    
    return available;
}
