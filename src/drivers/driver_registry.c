/**
 * @file driver_registry.c
 * @brief Generic Driver Registration System Implementation
 */

#include "driver_registry.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(driver_registry, LOG_LEVEL_INF);

#define MAX_DRIVERS 32

static struct {
    bool initialized;
    driver_desc_t drivers[MAX_DRIVERS];
    int count;
    struct k_mutex mutex;
} registry = {0};

int driver_registry_init(void)
{
    if (registry.initialized) {
        return 0;
    }

    LOG_INF("Initializing driver registry");
    
    k_mutex_init(&registry.mutex);
    memset(registry.drivers, 0, sizeof(registry.drivers));
    registry.count = 0;
    registry.initialized = true;
    
    return 0;
}

int driver_registry_register(const char *name, driver_type_t type, 
                             const driver_ops_t *ops)
{
    if (!registry.initialized || !name || !ops) {
        return -EINVAL;
    }

    k_mutex_lock(&registry.mutex, K_FOREVER);

    /* Check if already registered */
    for (int i = 0; i < registry.count; i++) {
        if (strcmp(registry.drivers[i].name, name) == 0) {
            LOG_WRN("Driver '%s' already registered", name);
            k_mutex_unlock(&registry.mutex);
            return -EEXIST;
        }
    }

    /* Find free slot */
    if (registry.count >= MAX_DRIVERS) {
        LOG_ERR("Driver registry full");
        k_mutex_unlock(&registry.mutex);
        return -ENOMEM;
    }

    /* Register driver */
    driver_desc_t *desc = &registry.drivers[registry.count];
    desc->name = name;
    desc->type = type;
    desc->ops = ops;
    desc->initialized = false;

    registry.count++;

    LOG_INF("Registered driver '%s' (type=%d, total=%d)", 
            name, type, registry.count);

    k_mutex_unlock(&registry.mutex);
    return 0;
}

int driver_registry_unregister(const char *name)
{
    if (!registry.initialized || !name) {
        return -EINVAL;
    }

    k_mutex_lock(&registry.mutex, K_FOREVER);

    /* Find driver */
    int idx = -1;
    for (int i = 0; i < registry.count; i++) {
        if (strcmp(registry.drivers[i].name, name) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        k_mutex_unlock(&registry.mutex);
        return -ENOENT;
    }

    /* Deinit if needed */
    if (registry.drivers[idx].initialized && 
        registry.drivers[idx].ops->deinit) {
        registry.drivers[idx].ops->deinit();
    }

    /* Shift remaining drivers */
    for (int i = idx; i < registry.count - 1; i++) {
        registry.drivers[i] = registry.drivers[i + 1];
    }
    registry.count--;

    LOG_INF("Unregistered driver '%s'", name);

    k_mutex_unlock(&registry.mutex);
    return 0;
}

const driver_desc_t *driver_registry_get(const char *name)
{
    if (!registry.initialized || !name) {
        return NULL;
    }

    k_mutex_lock(&registry.mutex, K_FOREVER);

    for (int i = 0; i < registry.count; i++) {
        if (strcmp(registry.drivers[i].name, name) == 0) {
            k_mutex_unlock(&registry.mutex);
            return &registry.drivers[i];
        }
    }

    k_mutex_unlock(&registry.mutex);
    return NULL;
}

const driver_desc_t *driver_registry_get_by_type(driver_type_t type)
{
    if (!registry.initialized) {
        return NULL;
    }

    k_mutex_lock(&registry.mutex, K_FOREVER);

    for (int i = 0; i < registry.count; i++) {
        if (registry.drivers[i].type == type) {
            k_mutex_unlock(&registry.mutex);
            return &registry.drivers[i];
        }
    }

    k_mutex_unlock(&registry.mutex);
    return NULL;
}

int driver_registry_list(driver_type_t type, const char **names, size_t max_count)
{
    if (!registry.initialized || !names) {
        return -EINVAL;
    }

    k_mutex_lock(&registry.mutex, K_FOREVER);

    int found = 0;
    for (int i = 0; i < registry.count && found < max_count; i++) {
        if (type == DRIVER_TYPE_MAX || registry.drivers[i].type == type) {
            names[found++] = registry.drivers[i].name;
        }
    }

    k_mutex_unlock(&registry.mutex);
    return found;
}
