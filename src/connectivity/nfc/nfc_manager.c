/**
 * @file nfc_manager.c
 * @brief NFC tag abstraction layer — central registry for NFC hardware
 *
 * Mirrors radio_manager.c, trimmed to the minimal vtable this layer needs.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
 */

#include "connectivity/nfc_interface.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(nfc_manager, CONFIG_AKIRA_LOG_LEVEL);

#define MAX_NFC_DEVICES CONFIG_AKIRA_NFC_MAX_DEVICES

static struct {
    nfc_handle_t   *devices[MAX_NFC_DEVICES];  /* NULL = empty slot */
    struct k_mutex  lock;
    bool            initialized;
} nfc_registry;

int nfc_manager_init(void)
{
    if (nfc_registry.initialized) {
        LOG_WRN("NFC manager already initialized");
        return 0;
    }

    memset(&nfc_registry, 0, sizeof(nfc_registry));
    k_mutex_init(&nfc_registry.lock);
    nfc_registry.initialized = true;

    LOG_INF("NFC manager initialized");
    return 0;
}

int nfc_manager_register(nfc_handle_t *handle)
{
    if (!handle) return -EINVAL;
    if (!nfc_registry.initialized) return -ENODEV;

    k_mutex_lock(&nfc_registry.lock, K_FOREVER);
    int slot = -1;
    for (int i = 0; i < MAX_NFC_DEVICES; i++) {
        if (nfc_registry.devices[i] == handle) {
            k_mutex_unlock(&nfc_registry.lock);
            return -EALREADY;
        }
        if (slot < 0 && nfc_registry.devices[i] == NULL) {
            slot = i;
        }
    }
    if (slot < 0) {
        k_mutex_unlock(&nfc_registry.lock);
        LOG_ERR("NFC registry full (max %d)", MAX_NFC_DEVICES);
        return -ENOMEM;
    }
    if (!handle->lock.lock_count) {
        k_mutex_init(&handle->lock);
    }
    nfc_registry.devices[slot] = handle;
    k_mutex_unlock(&nfc_registry.lock);
    LOG_INF("Registered NFC device: %s", handle->name);
    return 0;
}

int nfc_manager_unregister(nfc_handle_t *handle)
{
    if (!handle) return -EINVAL;
    if (!nfc_registry.initialized) return -ENODEV;

    k_mutex_lock(&nfc_registry.lock, K_FOREVER);
    for (int i = 0; i < MAX_NFC_DEVICES; i++) {
        if (nfc_registry.devices[i] == handle) {
            nfc_registry.devices[i] = NULL;
            k_mutex_unlock(&nfc_registry.lock);
            return 0;
        }
    }
    k_mutex_unlock(&nfc_registry.lock);
    return -ENOENT;
}

nfc_handle_t *nfc_manager_get(nfc_type_t type)
{
    if (!nfc_registry.initialized) {
        return NULL;
    }

    k_mutex_lock(&nfc_registry.lock, K_FOREVER);

    nfc_handle_t *result = NULL;
    for (int i = 0; i < MAX_NFC_DEVICES; i++) {
        if (!nfc_registry.devices[i]) continue;
        if (type == NFC_TYPE_NONE || nfc_registry.devices[i]->type == type) {
            result = nfc_registry.devices[i];
            break;
        }
    }

    k_mutex_unlock(&nfc_registry.lock);
    return result;
}

nfc_handle_t *nfc_manager_get_by_name(const char *name)
{
    if (!nfc_registry.initialized || !name) {
        return NULL;
    }

    k_mutex_lock(&nfc_registry.lock, K_FOREVER);

    nfc_handle_t *result = NULL;
    for (int i = 0; i < MAX_NFC_DEVICES; i++) {
        if (!nfc_registry.devices[i]) continue;
        if (strcmp(nfc_registry.devices[i]->name, name) == 0) {
            result = nfc_registry.devices[i];
            break;
        }
    }

    k_mutex_unlock(&nfc_registry.lock);
    return result;
}

SYS_INIT(nfc_manager_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
