/**
 * @file thread_manager.c
 * @brief OpenThread Network Manager Implementation
 *
 * Foundation implementation for Thread mesh networking.
 * Full implementation requires OpenThread module in west.yml.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
*/

#include "connectivity/thread_manager.h"
#include "connectivity/radio_interface.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <string.h>

LOG_MODULE_REGISTER(thread_manager, CONFIG_AKIRA_LOG_LEVEL);

/* Thread manager state */
static struct {
    thread_config_t config;
    thread_stats_t stats;
    radio_handle_t *radio;
    bool initialized;
    bool started;
} thread_state;

int thread_manager_init(const thread_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }
    
    if (thread_state.initialized) {
        LOG_WRN("Thread manager already initialized");
        return -EALREADY;
    }
    
    /* Get 802.15.4 radio */
    thread_state.radio = radio_manager_get(RADIO_TYPE_802154);
    if (!thread_state.radio) {
        LOG_ERR("802.15.4 radio not available for Thread");
        return -ENODEV;
    }
    
    memcpy(&thread_state.config, config, sizeof(thread_config_t));
    memset(&thread_state.stats, 0, sizeof(thread_stats_t));
    
    thread_state.initialized = true;
    thread_state.stats.role = THREAD_ROLE_DISABLED;
    
    LOG_INF("Thread manager initialized (Network: %s, PAN: 0x%04x, Channel: %d)",
            config->network_name, config->panid, config->channel);
    
    return 0;
}

int thread_start(void)
{
    if (!thread_state.initialized) {
        return -ENODEV;
    }
    
    if (thread_state.started) {
        LOG_WRN("Thread already started");
        return 0;
    }
    
    LOG_INF("Starting Thread network");
    
    /* Configure 802.15.4 radio for Thread */
    radio_config_t radio_cfg = {
        .channel = thread_state.config.channel,
        .tx_power = 0,
        .auto_ack = true,
    };
    radio_configure(thread_state.radio, &radio_cfg);
    
    /* Initialize OpenThread stack (requires OpenThread module).
     * This would call: otInstance *instance = otInstanceInitSingle().
     * OpenThread is not integrated, so the network cannot actually start.
     * Do NOT mark started/attached — that reported a live Thread network that
     * does not exist. Fail loud with -ENOSYS. */
    thread_state.stats.role = THREAD_ROLE_DISABLED;
    LOG_WRN("Thread start unimplemented (OpenThread not integrated)");
    return -ENOSYS;
}

int thread_stop(void)
{
    if (!thread_state.started) {
        return 0;
    }
    
    LOG_INF("Stopping Thread network");
    
    thread_state.started = false;
    thread_state.stats.role = THREAD_ROLE_DISABLED;
    
    return 0;
}

int thread_get_stats(thread_stats_t *stats)
{
    if (!stats) {
        return -EINVAL;
    }
    
    if (!thread_state.initialized) {
        return -ENODEV;
    }
    
    memcpy(stats, &thread_state.stats, sizeof(thread_stats_t));
    return 0;
}

int thread_get_dataset(uint8_t *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0) {
        return -EINVAL;
    }
    
    if (!thread_state.initialized) {
        return -ENODEV;
    }
    
    ARG_UNUSED(buffer);
    ARG_UNUSED(buffer_len);

    /* Requires OpenThread (otDatasetGetActive). Not integrated — return
     * -ENOSYS rather than 0, which would hand back an empty/garbage dataset. */
    LOG_WRN("Thread dataset export is unimplemented (OpenThread not integrated)");
    return -ENOSYS;
}
