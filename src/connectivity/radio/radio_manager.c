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
#include <string.h>

LOG_MODULE_REGISTER(radio_manager, CONFIG_AKIRA_LOG_LEVEL);

/* Maximum number of radios that can be registered */
#define MAX_RADIOS 8

/* Radio registry */
static struct {
    radio_handle_t *radios[MAX_RADIOS];
    uint8_t count;
    struct k_mutex lock;
    bool initialized;
} radio_registry;

/* String conversion tables */
static const char *radio_type_strings[] = {
    [RADIO_TYPE_NONE] = "None",
    [RADIO_TYPE_WIFI] = "WiFi",
    [RADIO_TYPE_BLE] = "BLE",
    [RADIO_TYPE_802154] = "802.15.4",
    [RADIO_TYPE_LORA] = "LoRa",
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
    if (!handle) {
        return -EINVAL;
    }
    
    if (!radio_registry.initialized) {
        LOG_ERR("Radio manager not initialized");
        return -ENODEV;
    }
    
    k_mutex_lock(&radio_registry.lock, K_FOREVER);
    
    /* Check if already registered */
    for (uint8_t i = 0; i < radio_registry.count; i++) {
        if (radio_registry.radios[i] == handle) {
            k_mutex_unlock(&radio_registry.lock);
            LOG_WRN("Radio %s already registered", handle->name);
            return -EALREADY;
        }
    }
    
    /* Check capacity */
    if (radio_registry.count >= MAX_RADIOS) {
        k_mutex_unlock(&radio_registry.lock);
        LOG_ERR("Radio registry full (max %d)", MAX_RADIOS);
        return -ENOMEM;
    }
    
    /* Initialize handle mutex if not already done */
    if (!handle->lock.lock_count) {
        k_mutex_init(&handle->lock);
    }
    
    /* Register radio */
    radio_registry.radios[radio_registry.count++] = handle;
    
    k_mutex_unlock(&radio_registry.lock);
    
    LOG_INF("Registered radio: %s (type=%s, caps=0x%08x)", 
            handle->name,
            radio_type_to_string(handle->type),
            handle->capabilities);
    
    return 0;
}

int radio_manager_unregister(radio_handle_t *handle)
{
    if (!handle) {
        return -EINVAL;
    }
    
    if (!radio_registry.initialized) {
        return -ENODEV;
    }
    
    k_mutex_lock(&radio_registry.lock, K_FOREVER);
    
    /* Find and remove radio */
    bool found = false;
    for (uint8_t i = 0; i < radio_registry.count; i++) {
        if (radio_registry.radios[i] == handle) {
            /* Shift remaining entries */
            for (uint8_t j = i; j < radio_registry.count - 1; j++) {
                radio_registry.radios[j] = radio_registry.radios[j + 1];
            }
            radio_registry.radios[--radio_registry.count] = NULL;
            found = true;
            break;
        }
    }
    
    k_mutex_unlock(&radio_registry.lock);
    
    if (!found) {
        LOG_WRN("Radio %s not found in registry", handle->name);
        return -ENOENT;
    }
    
    LOG_INF("Unregistered radio: %s", handle->name);
    return 0;
}

radio_handle_t *radio_manager_get(radio_type_t type)
{
    if (!radio_registry.initialized || type == RADIO_TYPE_NONE) {
        return NULL;
    }
    
    k_mutex_lock(&radio_registry.lock, K_FOREVER);
    
    radio_handle_t *result = NULL;
    for (uint8_t i = 0; i < radio_registry.count; i++) {
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
    for (uint8_t i = 0; i < radio_registry.count && count < max_handles; i++) {
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
