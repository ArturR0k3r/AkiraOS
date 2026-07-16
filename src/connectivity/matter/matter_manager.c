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

#ifdef CONFIG_AKIRA_MATTER_ACCESSORY
#include <runtime/akira_matter_ipc.h>

/* Cached onboarding payload fetched from the co-processor. */
static char s_qr_cache[AKIRA_MATTER_IPC_QR_LEN];
static char s_manual_cache[AKIRA_MATTER_IPC_MANUAL_LEN];
static bool s_onboarding_valid;

/* Fetch (and cache) this node's QR + manual code from the co-processor. */
static int accessory_refresh_onboarding(void)
{
    int rc = akira_matter_ipc_init();
    if (rc != 0) {
        return rc;
    }
    rc = akira_matter_ipc_get_qr(s_qr_cache, sizeof(s_qr_cache),
                                 s_manual_cache, sizeof(s_manual_cache));
    s_onboarding_valid = (rc == 0);
    return rc;
}
#endif /* CONFIG_AKIRA_MATTER_ACCESSORY */

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

#ifdef CONFIG_AKIRA_MATTER_ACCESSORY
    /* Accessory mode: ask the co-processor to open the real commissioning
     * window and refresh our onboarding payload. */
    int rc = akira_matter_ipc_init();
    if (rc == 0) {
        rc = akira_matter_ipc_open_pairing((uint16_t)timeout_sec);
    }
    if (rc != 0) {
        LOG_ERR("matter: co-processor pairing open failed (%d)", rc);
        matter_state.stats.state = MATTER_COMM_STATE_ERROR;
        return rc;
    }
    (void)accessory_refresh_onboarding();
#else
    /* Start BLE advertising for commissioning */
    radio_handle_t *ble_radio = radio_manager_get(RADIO_TYPE_BLE);
    if (ble_radio) {
        /* Configure BLE for Matter commissioning */
        /* This would start BLE advertising with Matter service UUID */
        LOG_INF("BLE commissioning enabled");
    }
#endif

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

#ifdef CONFIG_AKIRA_MATTER_ACCESSORY
    /* Real onboarding payload comes from the co-processor. */
    if (!s_onboarding_valid) {
        int rc = accessory_refresh_onboarding();
        if (rc != 0) {
            return rc;
        }
    }
    strncpy(buffer, s_qr_cache, buffer_len - 1);
    buffer[buffer_len - 1] = '\0';
#else
    /* No real commissioning payload exists without the co-processor
     * (CONFIG_AKIRA_MATTER_ACCESSORY). The previous code returned a hardcoded
     * example QR string, which would send a user through a commissioning flow
     * that cannot succeed. Fail with -ENOSYS instead. */
    ARG_UNUSED(buffer);
    LOG_WRN("Matter QR code unavailable: co-processor accessory path not enabled");
    return -ENOSYS;
#endif

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

#ifdef CONFIG_AKIRA_MATTER_ACCESSORY
    if (!s_onboarding_valid) {
        int rc = accessory_refresh_onboarding();
        if (rc != 0) {
            return rc;
        }
    }
    strncpy(buffer, s_manual_cache, buffer_len - 1);
    buffer[buffer_len - 1] = '\0';
#else
    /* Without the co-processor accessory path there is no real setup PIN /
     * discriminator, so any manual code would be non-functional. Fail loud. */
    ARG_UNUSED(buffer);
    LOG_WRN("Matter manual code unavailable: co-processor accessory path not enabled");
    return -ENOSYS;
#endif

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
    
    /* Reading a real Matter cluster attribute is not implemented
     * (would call e.g. chip::app::Clusters::OnOff::Attributes::OnOff::Get).
     * Returning a hardcoded 0 masqueraded as a real attribute value; fail with
     * -ENOSYS so callers do not act on fabricated cluster state. */
    LOG_WRN("Matter get-attribute unimplemented (EP%d Cluster0x%08x Attr0x%08x)",
            endpoint, cluster, attribute);
    return -ENOSYS;
}
