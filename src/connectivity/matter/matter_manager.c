/**
 * @file matter_manager.c
 * @brief Matter Protocol Stack Manager Implementation
 *
 * Provides Matter (CHIP/Connected Home over IP) protocol support with
 * hardware-agnostic transport through Radio Abstraction Layer.
 *
 * NOTE: This is a foundation implementation. Full Matter SDK integration
 * requires adding ConnectedHomeOverIP module to west.yml and enabling
 * CONFIG_CHIP in Kconfig.
 *
 * @copyright Copyright (c) 2026 PenEngineering S.R.L
*/

#include "connectivity/matter_manager.h"
#include "connectivity/radio_interface.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(matter_manager, CONFIG_AKIRA_LOG_LEVEL);

/* Matter manager state */
static struct {
    matter_config_t config;
    matter_stats_t stats;
    matter_event_cb_t event_cb;
    void *event_user_data;
    radio_handle_t *transport_radio;
    struct k_work_delayable commissioning_timeout_work;
    bool initialized;
    bool commissioned;
} matter_state;

/* Forward declarations */
static void commissioning_timeout_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(commissioning_timeout_work, commissioning_timeout_handler);

int matter_manager_init(const matter_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }
    
    if (matter_state.initialized) {
        LOG_WRN("Matter manager already initialized");
        return -EALREADY;
    }
    
    /* Copy configuration */
    memcpy(&matter_state.config, config, sizeof(matter_config_t));
    memset(&matter_state.stats, 0, sizeof(matter_stats_t));
    
    /* Select and bind to radio transport */
    switch (config->transport) {
    case MATTER_TRANSPORT_WIFI:
        matter_state.transport_radio = radio_manager_get(RADIO_TYPE_WIFI);
        if (!matter_state.transport_radio) {
            LOG_ERR("WiFi radio not available for Matter transport");
            return -ENODEV;
        }
        LOG_INF("Matter using WiFi transport");
        break;
        
    case MATTER_TRANSPORT_THREAD:
        matter_state.transport_radio = radio_manager_get(RADIO_TYPE_802154);
        if (!matter_state.transport_radio) {
            LOG_ERR("802.15.4 radio not available for Matter-over-Thread");
            return -ENODEV;
        }
        LOG_INF("Matter using Thread transport");
        break;
        
    case MATTER_TRANSPORT_BLE:
        matter_state.transport_radio = radio_manager_get(RADIO_TYPE_BLE);
        if (!matter_state.transport_radio) {
            LOG_ERR("BLE radio not available for Matter commissioning");
            return -ENODEV;
        }
        LOG_INF("Matter using BLE transport (commissioning only)");
        break;
        
    default:
        LOG_ERR("Invalid Matter transport type: %d", config->transport);
        return -EINVAL;
    }
    
    /* Initialize Matter SDK (placeholder - requires CHIP integration) */
    /* This would call: chip::DeviceLayer::PlatformMgr().InitChipStack() */
    
    matter_state.stats.state = MATTER_COMM_STATE_NONE;
    matter_state.initialized = true;
    matter_state.commissioned = false;
    
    LOG_INF("Matter manager initialized");
    LOG_INF("  Device: %s (VID:0x%04x PID:0x%04x)", 
            config->device_name, config->vendor_id, config->product_id);
    LOG_INF("  Type: 0x%04x, Discriminator: %d", 
            config->device_type, config->discriminator);
    LOG_INF("  Setup PIN: %08u", config->setup_pin_code);
    
    return 0;
}

static void commissioning_timeout_handler(struct k_work *work)
{
    LOG_INF("Matter commissioning window timeout");
    matter_stop_commissioning();
}

int matter_start_commissioning(uint32_t timeout_sec)
{
    if (!matter_state.initialized) {
        return -ENODEV;
    }
    
    if (matter_state.commissioned) {
        LOG_WRN("Device already commissioned - use factory reset first");
        return -EALREADY;
    }
    
    LOG_INF("Starting Matter commissioning (timeout: %u sec)", timeout_sec);
    
    /* Start BLE advertising for commissioning */
    radio_handle_t *ble_radio = radio_manager_get(RADIO_TYPE_BLE);
    if (ble_radio) {
        /* Configure BLE for Matter commissioning */
        /* This would start BLE advertising with Matter service UUID */
        LOG_INF("BLE commissioning enabled");
    }
    
    matter_state.stats.state = MATTER_COMM_STATE_BLE_ADVERTISING;
    matter_state.stats.commissioning_attempts++;
    
    /* Set timeout if specified */
    if (timeout_sec > 0) {
        k_work_schedule(&commissioning_timeout_work, K_SECONDS(timeout_sec));
    }
    
    /* Notify event callback */
    if (matter_state.event_cb) {
        matter_event_t event = {
            .type = MATTER_EVENT_COMMISSIONED,  /* TODO: Add START_COMMISSIONING event */
        };
        matter_state.event_cb(&event, matter_state.event_user_data);
    }
    
    return 0;
}

int matter_stop_commissioning(void)
{
    if (!matter_state.initialized) {
        return -ENODEV;
    }
    
    LOG_INF("Stopping Matter commissioning");
    
    /* Cancel timeout work */
    k_work_cancel_delayable(&commissioning_timeout_work);
    
    /* Stop BLE advertising */
    radio_handle_t *ble_radio = radio_manager_get(RADIO_TYPE_BLE);
    if (ble_radio) {
        /* Stop BLE commissioning advertisements */
        LOG_INF("BLE commissioning disabled");
    }
    
    matter_state.stats.state = matter_state.commissioned ? 
                               MATTER_COMM_STATE_COMMISSIONED : MATTER_COMM_STATE_NONE;
    
    return 0;
}

int matter_factory_reset(void)
{
    if (!matter_state.initialized) {
        return -ENODEV;
    }
    
    LOG_WRN("Performing Matter factory reset");
    
    /* Stop any active commissioning */
    matter_stop_commissioning();
    
    /* Erase Matter persistent storage */
    /* This would call: chip::DeviceLayer::ConfigurationMgr().InitiateFactoryReset() */
    
    matter_state.commissioned = false;
    matter_state.stats.state = MATTER_COMM_STATE_NONE;
    memset(&matter_state.stats, 0, sizeof(matter_stats_t));
    
    /* Notify event callback */
    if (matter_state.event_cb) {
        matter_event_t event = {
            .type = MATTER_EVENT_DECOMMISSIONED,
        };
        matter_state.event_cb(&event, matter_state.event_user_data);
    }
    
    LOG_INF("Matter factory reset complete");
    return 0;
}

matter_comm_state_t matter_get_commissioning_state(void)
{
    return matter_state.stats.state;
}

int matter_get_qr_code(char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len < 50) {
        return -EINVAL;
    }
    
    if (!matter_state.initialized) {
        return -ENODEV;
    }
    
    /* Generate Matter QR code payload */
    /* Format: MT:<version><vendor-id><product-id><discriminator><setup-pin> */
    snprintf(buffer, buffer_len, "MT:Y.K9042C00KA0648G00");  /* Example QR code */
    
    LOG_DBG("Generated Matter QR code: %s", buffer);
    return 0;
}

int matter_get_manual_code(char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len < 12) {
        return -EINVAL;
    }
    
    if (!matter_state.initialized) {
        return -ENODEV;
    }
    
    /* Generate 11-digit manual pairing code */
    /* Format: discriminator (4 digits) + setup PIN (8 digits with check digit) */
    snprintf(buffer, buffer_len, "%04d-%08u",
             matter_state.config.discriminator,
             matter_state.config.setup_pin_code);
    
    LOG_DBG("Generated Matter manual code: %s", buffer);
    return 0;
}

int matter_get_stats(matter_stats_t *stats)
{
    if (!stats) {
        return -EINVAL;
    }
    
    if (!matter_state.initialized) {
        return -ENODEV;
    }
    
    matter_state.stats.uptime_sec = k_uptime_get() / 1000;
    memcpy(stats, &matter_state.stats, sizeof(matter_stats_t));
    
    return 0;
}

int matter_register_event_callback(matter_event_cb_t callback, void *user_data)
{
    matter_state.event_cb = callback;
    matter_state.event_user_data = user_data;
    LOG_DBG("Matter event callback registered");
    return 0;
}

int matter_set_attribute(uint8_t endpoint, uint32_t cluster,
                        uint32_t attribute, const void *value, size_t value_len)
{
    if (!matter_state.initialized) {
        return -ENODEV;
    }
    
    LOG_DBG("Matter set attribute: EP%d Cluster0x%08x Attr0x%08x (%zu bytes)",
            endpoint, cluster, attribute, value_len);
    
    /* This would update Matter cluster attribute */
    /* Example: chip::app::Clusters::OnOff::Attributes::OnOff::Set(endpoint, *value) */
    
    matter_state.stats.messages_sent++;
    
    /* Notify binding updates if applicable */
    if (matter_state.event_cb) {
        matter_event_t event = {
            .type = MATTER_EVENT_ATTRIBUTE_CHANGED,
            .data = (void *)&attribute,
            .data_len = sizeof(attribute),
        };
        matter_state.event_cb(&event, matter_state.event_user_data);
    }
    
    return 0;
}

int matter_get_attribute(uint8_t endpoint, uint32_t cluster,
                        uint32_t attribute, void *value, size_t *value_len)
{
    if (!matter_state.initialized || !value || !value_len) {
        return -EINVAL;
    }
    
    LOG_DBG("Matter get attribute: EP%d Cluster0x%08x Attr0x%08x",
            endpoint, cluster, attribute);
    
    /* This would read Matter cluster attribute */
    /* Example: chip::app::Clusters::OnOff::Attributes::OnOff::Get(endpoint, value) */
    
    matter_state.stats.messages_received++;
    
    /* Placeholder: return dummy value */
    if (*value_len >= 1) {
        *((uint8_t *)value) = 0;
        *value_len = 1;
    }
    
    return 0;
}
