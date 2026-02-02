/**
 * @file transport_interface.c
 * @brief Callback-based Transport Layer Implementation
 *
 * Thread-safe registry with O(1) lookup for up to 8 handlers.
 * Supports zero-copy data dispatch with priority-based handler ordering.
 */

#include "transport_interface.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <string.h>

LOG_MODULE_REGISTER(transport, CONFIG_LOG_DEFAULT_LEVEL);

/* Maximum handlers per data type (2 handlers × 4 types = 8 total) */
#define HANDLERS_PER_TYPE 2
#define MAX_HANDLERS (TRANSPORT_DATA_TYPE_COUNT * HANDLERS_PER_TYPE)

/* Handler registry - O(1) lookup via type index */
static struct {
    struct transport_handler handlers[HANDLERS_PER_TYPE];
    uint8_t count;
    bool transfer_active;
    uint32_t current_offset;
    uint32_t total_size;
    const char *transfer_name;
} registry[TRANSPORT_DATA_TYPE_COUNT];

/* Global statistics */
static struct transport_stats global_stats;
static struct transport_stats type_stats[TRANSPORT_DATA_TYPE_COUNT];

/* Mutex for thread safety */
static K_MUTEX_DEFINE(transport_mutex);

/* Initialization flag */
static bool initialized = false;

static const char *type_strings[] = {
    [TRANSPORT_DATA_WASM_APP] = "WASM_APP",
    [TRANSPORT_DATA_FIRMWARE] = "FIRMWARE",
    [TRANSPORT_DATA_FILE] = "FILE",
    [TRANSPORT_DATA_CONFIG] = "CONFIG",
};

const char *transport_type_to_string(enum transport_data_type type)
{
    if (type < TRANSPORT_DATA_TYPE_COUNT) {
        return type_strings[type];
    }
    return "UNKNOWN";
}

int transport_init(void)
{
    k_mutex_lock(&transport_mutex, K_FOREVER);

    if (initialized) {
        k_mutex_unlock(&transport_mutex);
        return 0;
    }

    /* Clear all registries */
    memset(registry, 0, sizeof(registry));
    memset(&global_stats, 0, sizeof(global_stats));
    memset(type_stats, 0, sizeof(type_stats));

    initialized = true;

    k_mutex_unlock(&transport_mutex);

    LOG_INF("Transport interface initialized (max %d handlers)", MAX_HANDLERS);
    return 0;
}

int transport_register_handler(enum transport_data_type type,
                               transport_data_cb_t callback,
                               void *user_data,
                               uint8_t priority)
{
    if (type >= TRANSPORT_DATA_TYPE_COUNT || !callback) {
        LOG_ERR("Invalid registration params: type=%d, cb=%p", type, callback);
        return -EINVAL;
    }

    k_mutex_lock(&transport_mutex, K_FOREVER);

    if (!initialized) {
        k_mutex_unlock(&transport_mutex);
        LOG_ERR("Transport not initialized");
        return -ENODEV;
    }

    /* Check for duplicate registration */
    for (int i = 0; i < registry[type].count; i++) {
        if (registry[type].handlers[i].callback == callback) {
            k_mutex_unlock(&transport_mutex);
            LOG_WRN("Callback already registered for type %s",
                    transport_type_to_string(type));
            return -EALREADY;
        }
    }

    /* Check if registry is full for this type */
    if (registry[type].count >= HANDLERS_PER_TYPE) {
        k_mutex_unlock(&transport_mutex);
        LOG_ERR("Registry full for type %s", transport_type_to_string(type));
        return -ENOSPC;
    }

    /* Find insertion point based on priority (lower = higher priority) */
    int insert_idx = registry[type].count;
    for (int i = 0; i < registry[type].count; i++) {
        if (priority < registry[type].handlers[i].priority) {
            /* Shift handlers down to make room */
            for (int j = registry[type].count; j > i; j--) {
                registry[type].handlers[j] = registry[type].handlers[j - 1];
            }
            insert_idx = i;
            break;
        }
    }

    /* Insert handler */
    registry[type].handlers[insert_idx].callback = callback;
    registry[type].handlers[insert_idx].user_data = user_data;
    registry[type].handlers[insert_idx].priority = priority;
    registry[type].handlers[insert_idx].active = true;
    registry[type].count++;

    /* Calculate global handler ID: type * HANDLERS_PER_TYPE + local_index */
    int handler_id = type * HANDLERS_PER_TYPE + insert_idx;

    k_mutex_unlock(&transport_mutex);

    LOG_INF("Registered handler for %s (id=%d, priority=%d)",
            transport_type_to_string(type), handler_id, priority);

    return handler_id;
}

int transport_unregister_handler(int handler_id)
{
    if (handler_id < 0 || handler_id >= MAX_HANDLERS) {
        return -EINVAL;
    }

    int type = handler_id / HANDLERS_PER_TYPE;
    int idx = handler_id % HANDLERS_PER_TYPE;

    k_mutex_lock(&transport_mutex, K_FOREVER);

    if (!initialized || type >= TRANSPORT_DATA_TYPE_COUNT ||
        idx >= registry[type].count) {
        k_mutex_unlock(&transport_mutex);
        return -ENOENT;
    }

    /* Shift remaining handlers up */
    for (int i = idx; i < registry[type].count - 1; i++) {
        registry[type].handlers[i] = registry[type].handlers[i + 1];
    }

    /* Clear last slot and decrement count */
    memset(&registry[type].handlers[registry[type].count - 1], 0,
           sizeof(struct transport_handler));
    registry[type].count--;

    k_mutex_unlock(&transport_mutex);

    LOG_INF("Unregistered handler id=%d from type %s",
            handler_id, transport_type_to_string(type));

    return 0;
}

int transport_notify(enum transport_data_type type,
                     const uint8_t *data,
                     size_t len,
                     const struct transport_chunk_info *info)
{
    if (type >= TRANSPORT_DATA_TYPE_COUNT) {
        return -EINVAL;
    }

    /* Allow NULL data only for abort/end notifications */
    if (!data && len > 0) {
        return -EINVAL;
    }

    uint32_t start_cycles = k_cycle_get_32();

    k_mutex_lock(&transport_mutex, K_FOREVER);

    if (!initialized) {
        k_mutex_unlock(&transport_mutex);
        return -ENODEV;
    }

    if (registry[type].count == 0) {
        k_mutex_unlock(&transport_mutex);
        LOG_DBG("No handlers for type %s", transport_type_to_string(type));
        return 0;  /* Not an error - just no handlers */
    }

    /* Build chunk info if not provided */
    struct transport_chunk_info local_info;
    if (!info) {
        memset(&local_info, 0, sizeof(local_info));
        local_info.type = type;
        local_info.offset = registry[type].current_offset;
        local_info.total_size = registry[type].total_size;
        local_info.name = registry[type].transfer_name;
        info = &local_info;
    }

    /* Dispatch to all handlers in priority order */
    int result = 0;
    for (int i = 0; i < registry[type].count; i++) {
        struct transport_handler *h = &registry[type].handlers[i];
        if (h->active && h->callback) {
            /* Create info with handler's user_data */
            struct transport_chunk_info handler_info = *info;
            handler_info.user_data = h->user_data;

            /* Unlock during callback to avoid deadlocks */
            k_mutex_unlock(&transport_mutex);

            int ret = h->callback(data, len, &handler_info);

            k_mutex_lock(&transport_mutex, K_FOREVER);

            if (ret != 0) {
                LOG_ERR("Handler %d failed for %s: %d",
                        i, transport_type_to_string(type), ret);
                type_stats[type].errors++;
                global_stats.errors++;
                result = ret;  /* Return first error but continue */
            }
        }
    }

    /* Update offset for next chunk */
    if (len > 0) {
        registry[type].current_offset += len;
        type_stats[type].total_bytes += len;
        type_stats[type].total_chunks++;
        global_stats.total_bytes += len;
        global_stats.total_chunks++;
    }

    /* Calculate dispatch latency */
    uint32_t elapsed_cycles = k_cycle_get_32() - start_cycles;
    uint32_t latency_us = k_cyc_to_us_near32(elapsed_cycles);
    type_stats[type].dispatch_latency_us = latency_us;
    global_stats.dispatch_latency_us = latency_us;

    k_mutex_unlock(&transport_mutex);

    return result;
}

int transport_begin(enum transport_data_type type,
                    uint32_t total_size,
                    const char *name)
{
    if (type >= TRANSPORT_DATA_TYPE_COUNT) {
        return -EINVAL;
    }

    k_mutex_lock(&transport_mutex, K_FOREVER);

    if (!initialized) {
        k_mutex_unlock(&transport_mutex);
        return -ENODEV;
    }

    if (registry[type].transfer_active) {
        k_mutex_unlock(&transport_mutex);
        LOG_WRN("Transfer already active for %s", transport_type_to_string(type));
        return -EBUSY;
    }

    registry[type].transfer_active = true;
    registry[type].current_offset = 0;
    registry[type].total_size = total_size;
    registry[type].transfer_name = name;

    k_mutex_unlock(&transport_mutex);

    LOG_INF("Transfer started: type=%s, size=%u, name=%s",
            transport_type_to_string(type), total_size,
            name ? name : "(null)");

    /* Notify handlers of transfer start */
    struct transport_chunk_info info = {
        .type = type,
        .total_size = total_size,
        .offset = 0,
        .flags = TRANSPORT_FLAG_CHUNK_START,
        .name = name,
    };

    return transport_notify(type, NULL, 0, &info);
}

int transport_end(enum transport_data_type type, bool success)
{
    if (type >= TRANSPORT_DATA_TYPE_COUNT) {
        return -EINVAL;
    }

    k_mutex_lock(&transport_mutex, K_FOREVER);

    if (!initialized) {
        k_mutex_unlock(&transport_mutex);
        return -ENODEV;
    }

    if (!registry[type].transfer_active) {
        k_mutex_unlock(&transport_mutex);
        return 0;  /* Not an error */
    }

    uint32_t final_offset = registry[type].current_offset;
    const char *name = registry[type].transfer_name;

    registry[type].transfer_active = false;
    registry[type].current_offset = 0;
    registry[type].total_size = 0;
    registry[type].transfer_name = NULL;

    k_mutex_unlock(&transport_mutex);

    LOG_INF("Transfer ended: type=%s, bytes=%u, success=%d",
            transport_type_to_string(type), final_offset, success);

    /* Notify handlers of transfer end */
    struct transport_chunk_info info = {
        .type = type,
        .total_size = final_offset,
        .offset = final_offset,
        .flags = TRANSPORT_FLAG_CHUNK_END | (success ? 0 : TRANSPORT_FLAG_ABORT),
        .name = name,
    };

    return transport_notify(type, NULL, 0, &info);
}

int transport_abort(enum transport_data_type type)
{
    if (type >= TRANSPORT_DATA_TYPE_COUNT) {
        return -EINVAL;
    }

    k_mutex_lock(&transport_mutex, K_FOREVER);

    if (!initialized) {
        k_mutex_unlock(&transport_mutex);
        return -ENODEV;
    }

    bool was_active = registry[type].transfer_active;
    const char *name = registry[type].transfer_name;

    registry[type].transfer_active = false;
    registry[type].current_offset = 0;
    registry[type].total_size = 0;
    registry[type].transfer_name = NULL;

    k_mutex_unlock(&transport_mutex);

    if (was_active) {
        LOG_WRN("Transfer aborted: type=%s", transport_type_to_string(type));

        /* Notify handlers of abort */
        struct transport_chunk_info info = {
            .type = type,
            .flags = TRANSPORT_FLAG_ABORT,
            .name = name,
        };

        return transport_notify(type, NULL, 0, &info);
    }

    return 0;
}

bool transport_is_active(enum transport_data_type type)
{
    if (type >= TRANSPORT_DATA_TYPE_COUNT) {
        return false;
    }

    k_mutex_lock(&transport_mutex, K_FOREVER);
    bool active = initialized && registry[type].transfer_active;
    k_mutex_unlock(&transport_mutex);

    return active;
}

int transport_get_stats(int type, struct transport_stats *stats)
{
    if (!stats) {
        return -EINVAL;
    }

    k_mutex_lock(&transport_mutex, K_FOREVER);

    if (!initialized) {
        k_mutex_unlock(&transport_mutex);
        return -ENODEV;
    }

    if (type < 0) {
        /* Return aggregate stats */
        memcpy(stats, &global_stats, sizeof(*stats));
    } else if (type < TRANSPORT_DATA_TYPE_COUNT) {
        /* Return type-specific stats */
        memcpy(stats, &type_stats[type], sizeof(*stats));
    } else {
        k_mutex_unlock(&transport_mutex);
        return -EINVAL;
    }

    k_mutex_unlock(&transport_mutex);

    return 0;
}
