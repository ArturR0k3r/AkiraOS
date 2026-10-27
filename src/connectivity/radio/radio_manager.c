/**
 * @file radio_manager.c
 * @brief Radio Abstraction Layer (RAL) Manager Implementation
 *
 * Central registry and management for all radio hardware in the system.
 * Provides hardware-agnostic access to WiFi, BLE, and 802.15.4 radios.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
*/

#include "connectivity/radio_interface.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(radio_manager, CONFIG_AKIRA_LOG_LEVEL);

/* Maximum number of radios that can be registered */
#define MAX_RADIOS CONFIG_AKIRA_RADIO_MAX_RADIOS
/* Radio registry */
static struct {
    radio_handle_t *radios[MAX_RADIOS];  /* NULL = empty slot */
    const char     *owners[MAX_RADIOS];  /* NULL = unacquired; aligned with radios[] */
    struct k_mutex  lock;
    bool            initialized;
} radio_registry;

/* String conversion tables */
static const char *radio_type_strings[] = {
    [RADIO_TYPE_NONE] = "None",
    [RADIO_TYPE_WIFI] = "WiFi",
    [RADIO_TYPE_BLE] = "BLE",
    [RADIO_TYPE_802154] = "802.15.4",
    [RADIO_TYPE_LORA]   = "LoRa",
    [RADIO_TYPE_SUBGHZ] = "SubGHz",
};

static const char *radio_state_strings[] = {
    [RADIO_STATE_OFF] = "Off",
    [RADIO_STATE_IDLE] = "Idle",
    [RADIO_STATE_RX] = "RX",
    [RADIO_STATE_TX] = "TX",
    [RADIO_STATE_SCAN] = "Scanning",
    [RADIO_STATE_SLEEP] = "Sleep",
    [RADIO_STATE_ERROR] = "Error",
};

int radio_manager_init(void)
{
    if (radio_registry.initialized) {
        LOG_WRN("Radio manager already initialized");
        return 0;
    }
    
    memset(&radio_registry, 0, sizeof(radio_registry));
    k_mutex_init(&radio_registry.lock);
    radio_registry.initialized = true;
    
    LOG_INF("Radio manager initialized");
    return 0;
}

int radio_manager_register(radio_handle_t *handle)
{
    if (!handle) return -EINVAL;
    if (!radio_registry.initialized) return -ENODEV;

    k_mutex_lock(&radio_registry.lock, K_FOREVER);
    int slot = -1;
    for (int i = 0; i < MAX_RADIOS; i++) {
        if (radio_registry.radios[i] == handle) {
            k_mutex_unlock(&radio_registry.lock);
            return -EALREADY;
        }
        if (slot < 0 && radio_registry.radios[i] == NULL) slot = i;
    }
    if (slot < 0) {
        k_mutex_unlock(&radio_registry.lock);
        LOG_ERR("Radio registry full (max %d)", MAX_RADIOS);
        return -ENOMEM;
    }
    if (!handle->lock.lock_count) k_mutex_init(&handle->lock);
    radio_registry.radios[slot] = handle;
    radio_registry.owners[slot] = NULL;
    k_mutex_unlock(&radio_registry.lock);
    LOG_INF("Registered radio: %s (caps=0x%08x)", handle->name, handle->capabilities);
    return 0;
}

int radio_manager_unregister(radio_handle_t *handle)
{
    if (!handle) return -EINVAL;
    if (!radio_registry.initialized) return -ENODEV;

    k_mutex_lock(&radio_registry.lock, K_FOREVER);
    for (int i = 0; i < MAX_RADIOS; i++) {
        if (radio_registry.radios[i] == handle) {
            radio_registry.radios[i] = NULL;
            radio_registry.owners[i] = NULL;
            k_mutex_unlock(&radio_registry.lock);
            return 0;
        }
    }
    k_mutex_unlock(&radio_registry.lock);
    return -ENOENT;
}

radio_handle_t *radio_manager_get(radio_type_t type)
{
    if (!radio_registry.initialized || type == RADIO_TYPE_NONE) {
        return NULL;
    }
    
    k_mutex_lock(&radio_registry.lock, K_FOREVER);
    
    radio_handle_t *result = NULL;
    for (int i = 0; i < MAX_RADIOS; i++) {
        if (!radio_registry.radios[i]) continue;
        if (radio_registry.radios[i]->type == type) {
            result = radio_registry.radios[i];
            break;
        }
    }
    
    k_mutex_unlock(&radio_registry.lock);
    
    if (!result) {
        LOG_DBG("Radio type %s not available", radio_type_to_string(type));
    }
    
    return result;
}

int radio_manager_get_all(radio_type_t type, radio_handle_t **handles, size_t max_handles)
{
    if (!handles || max_handles == 0) {
        return -EINVAL;
    }
    
    if (!radio_registry.initialized) {
        return -ENODEV;
    }
    
    k_mutex_lock(&radio_registry.lock, K_FOREVER);
    
    uint8_t count = 0;
    for (int i = 0; i < MAX_RADIOS && count < max_handles; i++) {
        if (!radio_registry.radios[i]) continue;
        if (type == RADIO_TYPE_NONE || radio_registry.radios[i]->type == type) {
            handles[count++] = radio_registry.radios[i];
        }
    }
    
    k_mutex_unlock(&radio_registry.lock);
    
    return count;
}

bool radio_manager_is_available(radio_type_t type)
{
    return radio_manager_get(type) != NULL;
}

radio_handle_t *radio_manager_get_by_name(const char *name)
{
    if (!radio_registry.initialized || !name) {
        return NULL;
    }

    k_mutex_lock(&radio_registry.lock, K_FOREVER);

    radio_handle_t *result = NULL;
    for (int i = 0; i < MAX_RADIOS; i++) {
        if (!radio_registry.radios[i]) continue;
        if (strcmp(radio_registry.radios[i]->name, name) == 0) {
            result = radio_registry.radios[i];
            break;
        }
    }

    k_mutex_unlock(&radio_registry.lock);
    return result;
}

radio_handle_t *radio_manager_get_by_caps(uint32_t required_caps)
{
    if (!radio_registry.initialized || required_caps == 0) {
        return NULL;
    }

    k_mutex_lock(&radio_registry.lock, K_FOREVER);

    radio_handle_t *result = NULL;
    for (int i = 0; i < MAX_RADIOS; i++) {
        if (!radio_registry.radios[i]) continue;
        if ((radio_registry.radios[i]->capabilities & required_caps) == required_caps) {
            result = radio_registry.radios[i];
            break;
        }
    }

    k_mutex_unlock(&radio_registry.lock);
    return result;
}

int radio_manager_get_all_by_caps(uint32_t required_caps, radio_handle_t **handles, size_t max_handles)
{
    if (!handles || max_handles == 0) {
        return -EINVAL;
    }

    if (!radio_registry.initialized) {
        return -ENODEV;
    }

    k_mutex_lock(&radio_registry.lock, K_FOREVER);

    uint8_t count = 0;
    for (int i = 0; i < MAX_RADIOS && count < max_handles; i++) {
        if (!radio_registry.radios[i]) continue;
        if ((radio_registry.radios[i]->capabilities & required_caps) == required_caps) {
            handles[count++] = radio_registry.radios[i];
        }
    }

    k_mutex_unlock(&radio_registry.lock);
    return count;
}

/* ===========================================================================
 * Ownership API
 * =========================================================================*/

static int find_slot(const radio_handle_t *handle)
{
    for (int i = 0; i < MAX_RADIOS; i++)
        if (radio_registry.radios[i] == handle) return i;
    return -1;
}

int radio_manager_acquire(radio_handle_t *handle, const char *owner)
{
    if (!handle || !owner) return -EINVAL;
    if (!radio_registry.initialized) return -ENODEV;
    k_mutex_lock(&radio_registry.lock, K_FOREVER);
    int i = find_slot(handle);
    if (i < 0) { k_mutex_unlock(&radio_registry.lock); return -ENOENT; }
    const char *cur = radio_registry.owners[i];
    if (cur && strcmp(cur, owner) != 0) {
        k_mutex_unlock(&radio_registry.lock);
        return -EBUSY;
    }
    radio_registry.owners[i] = owner;
    k_mutex_unlock(&radio_registry.lock);
    return 0;
}

static radio_handle_t *acquire_match(radio_type_t type, uint32_t caps,
                                     bool by_type, const char *owner)
{
    if (!owner || !radio_registry.initialized) return NULL;
    k_mutex_lock(&radio_registry.lock, K_FOREVER);
    radio_handle_t *result = NULL;
    for (int i = 0; i < MAX_RADIOS; i++) {
        radio_handle_t *h = radio_registry.radios[i];
        if (!h || radio_registry.owners[i]) continue;
        bool match = by_type ? (h->type == type)
                             : ((h->capabilities & caps) == caps);
        if (match) {
            radio_registry.owners[i] = owner;
            result = h;
            break;
        }
    }
    k_mutex_unlock(&radio_registry.lock);
    return result;
}

radio_handle_t *radio_manager_acquire_by_type(radio_type_t type, const char *owner)
{
    if (type == RADIO_TYPE_NONE) return NULL;
    return acquire_match(type, 0, true, owner);
}

radio_handle_t *radio_manager_acquire_by_caps(uint32_t caps, const char *owner)
{
    if (caps == 0) return NULL;
    return acquire_match(RADIO_TYPE_NONE, caps, false, owner);
}

int radio_manager_release(radio_handle_t *handle, const char *owner)
{
    if (!handle || !owner) return -EINVAL;
    if (!radio_registry.initialized) return -ENODEV;
    k_mutex_lock(&radio_registry.lock, K_FOREVER);
    int i = find_slot(handle);
    if (i < 0) { k_mutex_unlock(&radio_registry.lock); return -ENOENT; }
    if (!radio_registry.owners[i] || strcmp(radio_registry.owners[i], owner) != 0) {
        k_mutex_unlock(&radio_registry.lock);
        return -EPERM;
    }
    radio_registry.owners[i] = NULL;
    k_mutex_unlock(&radio_registry.lock);
    return 0;
}

const char *radio_manager_get_owner(const radio_handle_t *handle)
{
    if (!handle) return NULL;
    if (!radio_registry.initialized) return NULL;
    k_mutex_lock(&radio_registry.lock, K_FOREVER);
    int i = find_slot(handle);
    const char *o = (i >= 0) ? radio_registry.owners[i] : NULL;
    k_mutex_unlock(&radio_registry.lock);
    return o;
}


uint32_t radio_get_capabilities(const radio_handle_t *handle)
{
    return handle ? handle->capabilities : 0;
}

bool radio_has_capability(const radio_handle_t *handle, uint32_t capability)
{
    return handle && (handle->capabilities & capability) != 0;
}

const char *radio_type_to_string(radio_type_t type)
{
    if (type >= 0 && type < ARRAY_SIZE(radio_type_strings)) {
        return radio_type_strings[type];
    }
    return "Unknown";
}

const char *radio_state_to_string(radio_state_t state)
{
    if (state >= 0 && state < ARRAY_SIZE(radio_state_strings)) {
        return radio_state_strings[state];
    }
    return "Unknown";
}

SYS_INIT(radio_manager_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
