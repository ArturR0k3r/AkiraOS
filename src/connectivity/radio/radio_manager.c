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
/* Timeout acquiring a radio handle's bus lock in data-path functions */
#define RADIO_BUS_TIMEOUT_MS 2000

/* Radio registry */
static struct {
    radio_handle_t *radios[MAX_RADIOS];
    radio_handle_t *active;
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

radio_handle_t *radio_manager_get_by_name(const char *name)
{
    if (!radio_registry.initialized || !name) {
        return NULL;
    }

    k_mutex_lock(&radio_registry.lock, K_FOREVER);

    radio_handle_t *result = NULL;
    for (uint8_t i = 0; i < radio_registry.count; i++) {
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
    for (uint8_t i = 0; i < radio_registry.count; i++) {
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
    for (uint8_t i = 0; i < radio_registry.count && count < max_handles; i++) {
        if ((radio_registry.radios[i]->capabilities & required_caps) == required_caps) {
            handles[count++] = radio_registry.radios[i];
        }
    }

    k_mutex_unlock(&radio_registry.lock);
    return count;
}

/* ===========================================================================
 * Active radio + data path
 * =========================================================================*/

int radio_manager_set_active(radio_handle_t *handle)
{
    k_mutex_lock(&radio_registry.lock, K_FOREVER);
    radio_registry.active = handle;
    k_mutex_unlock(&radio_registry.lock);
    return 0;
}

radio_handle_t *radio_manager_get_active(void)
{
    return radio_registry.active;
}

int radio_manager_send(const uint8_t *data, size_t len)
{
    radio_handle_t *h = radio_registry.active;
    if (!h || !h->ops || !h->ops->send) return -ENODEV;
    if (!data || len == 0) return -EINVAL;
    if (k_mutex_lock(&h->lock, K_MSEC(RADIO_BUS_TIMEOUT_MS)) != 0) return -EBUSY;
    int ret = h->ops->send(h, data, len);
    k_mutex_unlock(&h->lock);
    return ret;
}

int radio_manager_receive(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    radio_handle_t *h = radio_registry.active;
    if (!h || !h->ops || !h->ops->recv) return -ENODEV;
    if (!buf || max_len == 0) return -EINVAL;
    if (k_mutex_lock(&h->lock, K_MSEC(RADIO_BUS_TIMEOUT_MS)) != 0) return -EBUSY;
    int ret = h->ops->recv(h, buf, max_len, timeout_ms);
    k_mutex_unlock(&h->lock);
    return ret;
}

int radio_manager_set_frequency(uint32_t freq_hz)
{
    radio_handle_t *h = radio_registry.active;
    if (!h || !h->ops || !h->ops->set_frequency) return -ENODEV;
    if (k_mutex_lock(&h->lock, K_MSEC(RADIO_BUS_TIMEOUT_MS)) != 0) return -EBUSY;
    int ret = h->ops->set_frequency(h, freq_hz);
    k_mutex_unlock(&h->lock);
    return ret;
}

int radio_manager_set_power(int8_t dbm)
{
    radio_handle_t *h = radio_registry.active;
    if (!h || !h->ops || !h->ops->set_power) return -ENODEV;
    if (k_mutex_lock(&h->lock, K_MSEC(RADIO_BUS_TIMEOUT_MS)) != 0) return -EBUSY;
    int ret = h->ops->set_power(h, dbm);
    k_mutex_unlock(&h->lock);
    return ret;
}

int radio_manager_get_rssi(int16_t *rssi)
{
    if (!rssi) return -EINVAL;
    radio_handle_t *h = radio_registry.active;
    if (!h || !h->ops || !h->ops->get_rssi) {
        *rssi = RADIO_RSSI_UNAVAILABLE;
        return -ENODEV;
    }
    if (k_mutex_lock(&h->lock, K_MSEC(RADIO_BUS_TIMEOUT_MS)) != 0) {
        *rssi = RADIO_RSSI_UNAVAILABLE;
        return -EBUSY;
    }
    int ret = h->ops->get_rssi(h, rssi);
    if (ret == -ENOSYS) *rssi = RADIO_RSSI_UNAVAILABLE;
    k_mutex_unlock(&h->lock);
    return ret;
}

/* ===========================================================================
 * RX daemon + packet queue
 * =========================================================================*/

#ifdef CONFIG_AKIRA_RF_RX_DAEMON

#define RADIO_DAEMON_SLEEP_MS  100
#define RADIO_RX_MAX_PACKET    255

struct radio_rx_packet {
    uint8_t  data[RADIO_RX_MAX_PACKET];
    uint16_t len;
};

K_MSGQ_DEFINE(s_radio_rx_msgq, sizeof(struct radio_rx_packet),
              CONFIG_AKIRA_RF_RX_QUEUE_DEPTH, 4);

static struct radio_rx_packet s_rx_poll_buf;

static void radio_rx_daemon_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    while (1) {
        radio_handle_t *h = radio_registry.active;
        if (!h || !h->ops || !h->ops->recv) {
            k_msleep(RADIO_DAEMON_SLEEP_MS);
            continue;
        }

        /* Non-blocking lock — skip cycle if TX or manual recv in progress */
        if (k_mutex_lock(&h->lock, K_NO_WAIT) != 0) {
            LOG_DBG("daemon: %s busy, skip", h->name);
            k_msleep(RADIO_DAEMON_SLEEP_MS);
            continue;
        }

        int n = h->ops->recv(h, s_rx_poll_buf.data, RADIO_RX_MAX_PACKET,
                             CONFIG_AKIRA_RF_POLL_INTERVAL_MS);
        k_mutex_unlock(&h->lock);

        if (n <= 0) {
            k_msleep(RADIO_DAEMON_SLEEP_MS);
            continue;
        }

        s_rx_poll_buf.len = (uint16_t)n;
        if (k_msgq_put(&s_radio_rx_msgq, &s_rx_poll_buf, K_NO_WAIT) == -ENOMSG) {
            struct radio_rx_packet discard;
            k_msgq_get(&s_radio_rx_msgq, &discard, K_NO_WAIT);
            k_msgq_put(&s_radio_rx_msgq, &s_rx_poll_buf, K_NO_WAIT);
            LOG_DBG("radio RX queue full — dropped oldest");
        }
    }
}

K_THREAD_DEFINE(radio_rx_daemon, CONFIG_AKIRA_RF_DAEMON_STACK_SIZE,
                radio_rx_daemon_fn, NULL, NULL, NULL,
                CONFIG_AKIRA_RF_DAEMON_PRIORITY, 0, 0);

int radio_manager_recv_pop(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    if (!buf || max_len == 0) return -EINVAL;
    struct radio_rx_packet pkt;
    k_timeout_t t = (timeout_ms == 0) ? K_NO_WAIT : K_MSEC(timeout_ms);
    int ret = k_msgq_get(&s_radio_rx_msgq, &pkt, t);
    if (ret < 0) return ret;
    size_t copy = MIN(pkt.len, max_len);
    memcpy(buf, pkt.data, copy);
    return (int)copy;
}

#else

int radio_manager_recv_pop(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    ARG_UNUSED(buf); ARG_UNUSED(max_len); ARG_UNUSED(timeout_ms);
    return -ENOSYS;
}

#endif /* CONFIG_AKIRA_RF_RX_DAEMON */

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
