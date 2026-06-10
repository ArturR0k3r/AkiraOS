/**
 * @file rf_framework.c
 * @brief RF Driver Framework Implementation
 */

#include "rf_framework.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(rf_framework, LOG_LEVEL_INF);

/* Maximum number of RF drivers that can be registered */
#define RF_MAX_DRIVERS 8

/* Driver registry */
static struct {
    bool initialized;
    const struct akira_rf_driver *drivers[RF_MAX_DRIVERS];
    uint8_t driver_count;
    const struct akira_rf_driver *active_driver;
    struct k_mutex lock;
} g_rf_framework = {
    .initialized = false,
    .driver_count = 0,
    .active_driver = NULL,
};

int rf_framework_init(void)
{
    if (g_rf_framework.initialized) {
        LOG_DBG("RF framework already initialized");
        return 0;
    }

    /* Initialize mutex for thread safety */
    k_mutex_init(&g_rf_framework.lock);

    /* Clear driver registry */
    memset(g_rf_framework.drivers, 0, sizeof(g_rf_framework.drivers));
    g_rf_framework.driver_count = 0;
    g_rf_framework.active_driver = NULL;

    g_rf_framework.initialized = true;
    LOG_INF("RF framework initialized");

    return 0;
}

int rf_framework_register_driver(const struct akira_rf_driver *driver)
{
    if (!g_rf_framework.initialized) {
        LOG_ERR("RF framework not initialized");
        return -ENODEV;
    }

    if (!driver) {
        LOG_ERR("NULL driver pointer");
        return -EINVAL;
    }

    if (!driver->name || !driver->init || !driver->tx || !driver->rx) {
        LOG_ERR("Driver %s missing required functions", driver->name ? driver->name : "UNKNOWN");
        return -EINVAL;
    }

    k_mutex_lock(&g_rf_framework.lock, K_FOREVER);

    /* Check if already registered */
    for (int i = 0; i < g_rf_framework.driver_count; i++) {
        if (g_rf_framework.drivers[i]->type == driver->type) {
            LOG_WRN("Driver type %d already registered", driver->type);
            k_mutex_unlock(&g_rf_framework.lock);
            return -EEXIST;
        }
    }

    /* Check if registry is full */
    if (g_rf_framework.driver_count >= RF_MAX_DRIVERS) {
        LOG_ERR("Driver registry full (max %d)", RF_MAX_DRIVERS);
        k_mutex_unlock(&g_rf_framework.lock);
        return -ENOMEM;
    }

    /* Add driver to registry */
    g_rf_framework.drivers[g_rf_framework.driver_count] = driver;
    g_rf_framework.driver_count++;

    LOG_INF("Registered RF driver: %s (type=%d)", driver->name, driver->type);

    k_mutex_unlock(&g_rf_framework.lock);
    return 0;
}

const struct akira_rf_driver *rf_framework_get_driver(rf_chip_type_t type)
{
    if (!g_rf_framework.initialized) {
        LOG_ERR("RF framework not initialized");
        return NULL;
    }

    if (type == RF_CHIP_NONE) {
        return NULL;
    }

    k_mutex_lock(&g_rf_framework.lock, K_FOREVER);

    for (int i = 0; i < g_rf_framework.driver_count; i++) {
        if (g_rf_framework.drivers[i]->type == type) {
            const struct akira_rf_driver *drv = g_rf_framework.drivers[i];
            k_mutex_unlock(&g_rf_framework.lock);
            return drv;
        }
    }

    k_mutex_unlock(&g_rf_framework.lock);

    LOG_WRN("No driver found for chip type %d", type);
    return NULL;
}

const struct akira_rf_driver *rf_framework_get_active_driver(void)
{
    if (!g_rf_framework.initialized) {
        return NULL;
    }

    k_mutex_lock(&g_rf_framework.lock, K_FOREVER);
    const struct akira_rf_driver *drv = g_rf_framework.active_driver;
    k_mutex_unlock(&g_rf_framework.lock);

    return drv;
}

int rf_framework_set_active_driver(rf_chip_type_t type)
{
    if (!g_rf_framework.initialized) {
        LOG_ERR("RF framework not initialized");
        return -ENODEV;
    }

    const struct akira_rf_driver *drv = rf_framework_get_driver(type);
    if (!drv) {
        LOG_ERR("Driver type %d not found", type);
        return -ENOENT;
    }

    k_mutex_lock(&g_rf_framework.lock, K_FOREVER);
    g_rf_framework.active_driver = drv;
    k_mutex_unlock(&g_rf_framework.lock);

    LOG_INF("Active RF driver set to: %s", drv->name);
    return 0;
}

int rf_framework_get_driver_count(void)
{
    if (!g_rf_framework.initialized) {
        return 0;
    }

    k_mutex_lock(&g_rf_framework.lock, K_FOREVER);
    int count = g_rf_framework.driver_count;
    k_mutex_unlock(&g_rf_framework.lock);

    return count;
}

/* Shared RF reset line coordination. Set to 1 by the first init that claims it. */
static atomic_t g_shared_reset_claimed = ATOMIC_INIT(0);

bool rf_framework_claim_shared_reset(void)
{
    /* atomic_cas returns true only for the first caller (0 -> 1). */
    return atomic_cas(&g_shared_reset_claimed, 0, 1);
}

/* Auto-initialize RF framework at boot (before drivers register) */
SYS_INIT(rf_framework_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
