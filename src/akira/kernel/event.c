/**
 * @file event.c
 * @brief AkiraOS Event System Implementation
 */

#include "event.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_event, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#ifndef AKIRA_MAX_EVENT_HANDLERS
#define AKIRA_MAX_EVENT_HANDLERS 32
#endif

#ifndef AKIRA_EVENT_QUEUE_SIZE
#define AKIRA_EVENT_QUEUE_SIZE 32
#endif

/*===========================================================================*/
/* Internal Structures                                                       */
/*===========================================================================*/

typedef struct
{
    bool in_use;
    akira_event_type_t type_min;
    akira_event_type_t type_max;
    akira_event_handler_t handler;
    void *user_data;
} event_subscription_t;

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    event_subscription_t subscriptions[AKIRA_MAX_EVENT_HANDLERS];
    int subscription_count;
    akira_subscription_t next_id;

    /* Event queue */
    akira_event_t queue[AKIRA_EVENT_QUEUE_SIZE];
    int queue_head;
    int queue_tail;
    int queue_count;

    struct k_mutex mutex;
    struct k_sem queue_sem;
} event_sys;

/*===========================================================================*/
/* Internal Functions                                                        */
/*===========================================================================*/

static event_subscription_t *find_subscription(akira_subscription_t id)
{
    for (int i = 0; i < AKIRA_MAX_EVENT_HANDLERS; i++)
    {
        if (event_sys.subscriptions[i].in_use && i == id)
        {
            return &event_sys.subscriptions[i];
        }
    }
    return NULL;
}

static int find_free_slot(void)
{
    for (int i = 0; i < AKIRA_MAX_EVENT_HANDLERS; i++)
    {
        if (!event_sys.subscriptions[i].in_use)
        {
            return i;
        }
    }
    return -1;
}

static void deliver_event(const akira_event_t *event)
{
    for (int i = 0; i < AKIRA_MAX_EVENT_HANDLERS; i++)
    {
        event_subscription_t *sub = &event_sys.subscriptions[i];
        if (sub->in_use &&
            event->type >= sub->type_min &&
            event->type <= sub->type_max)
        {
            if (sub->handler)
            {
                int ret = sub->handler(event, sub->user_data);
                if (ret != 0)
                {
                    /* Handler requested stop propagation */
                    break;
                }
            }
        }
    }
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int akira_event_init(void)
{
    if (event_sys.initialized)
    {
        return 0;
    }

    LOG_INF("Initializing event system");

    k_mutex_init(&event_sys.mutex);
    k_sem_init(&event_sys.queue_sem, 0, AKIRA_EVENT_QUEUE_SIZE);

    memset(event_sys.subscriptions, 0, sizeof(event_sys.subscriptions));
    event_sys.subscription_count = 0;
    event_sys.next_id = 0;

    event_sys.queue_head = 0;
    event_sys.queue_tail = 0;
    event_sys.queue_count = 0;

    event_sys.initialized = true;

    LOG_INF("Event system initialized (handlers=%d, queue=%d)",
            AKIRA_MAX_EVENT_HANDLERS, AKIRA_EVENT_QUEUE_SIZE);

    return 0;
}

akira_subscription_t akira_event_subscribe(akira_event_type_t type,
                                           akira_event_handler_t handler,
                                           void *user_data)
{
    return akira_event_subscribe_range(type, type, handler, user_data);
}

akira_subscription_t akira_event_subscribe_range(akira_event_type_t type_min,
                                                 akira_event_type_t type_max,
                                                 akira_event_handler_t handler,
                                                 void *user_data)
{
    if (!event_sys.initialized || !handler)
    {
        return AKIRA_INVALID_HANDLE;
    }

    k_mutex_lock(&event_sys.mutex, K_FOREVER);

    int slot = find_free_slot();
    if (slot < 0)
    {
        k_mutex_unlock(&event_sys.mutex);
        LOG_ERR("No free subscription slots");
        return AKIRA_INVALID_HANDLE;
    }

    event_subscription_t *sub = &event_sys.subscriptions[slot];
    sub->in_use = true;
    sub->type_min = type_min;
    sub->type_max = type_max;
    sub->handler = handler;
    sub->user_data = user_data;

    event_sys.subscription_count++;

    k_mutex_unlock(&event_sys.mutex);

    LOG_DBG("Subscribed to events %d-%d (slot=%d)", type_min, type_max, slot);

    return slot;
}

int akira_event_unsubscribe(akira_subscription_t subscription)
{
    if (!event_sys.initialized || !AKIRA_HANDLE_VALID(subscription))
    {
        return -1;
    }

    k_mutex_lock(&event_sys.mutex, K_FOREVER);

    if (subscription >= AKIRA_MAX_EVENT_HANDLERS ||
        !event_sys.subscriptions[subscription].in_use)
    {
        k_mutex_unlock(&event_sys.mutex);
        return -1;
    }

    event_sys.subscriptions[subscription].in_use = false;
    event_sys.subscription_count--;

    k_mutex_unlock(&event_sys.mutex);

    LOG_DBG("Unsubscribed slot %d", subscription);

    return 0;
}

int akira_event_publish(const akira_event_t *event)
{
    if (!event_sys.initialized || !event)
    {
        return -1;
    }

    k_mutex_lock(&event_sys.mutex, K_FOREVER);
    deliver_event(event);
    k_mutex_unlock(&event_sys.mutex);

    return 0;
}

int akira_event_post(const akira_event_t *event)
{
    if (!event_sys.initialized || !event)
    {
        return -1;
    }

    k_mutex_lock(&event_sys.mutex, K_FOREVER);

    if (event_sys.queue_count >= AKIRA_EVENT_QUEUE_SIZE)
    {
        k_mutex_unlock(&event_sys.mutex);
        LOG_WRN("Event queue full, dropping event type %d", event->type);
        return -1;
    }

    /* Copy event to queue */
    event_sys.queue[event_sys.queue_tail] = *event;
    event_sys.queue_tail = (event_sys.queue_tail + 1) % AKIRA_EVENT_QUEUE_SIZE;
    event_sys.queue_count++;

    k_mutex_unlock(&event_sys.mutex);
    k_sem_give(&event_sys.queue_sem);

    return 0;
}

int akira_event_emit(akira_event_type_t type)
{
    akira_event_t event = AKIRA_EVENT(type);
    return akira_event_publish(&event);
}

int akira_event_process(void)
{
    if (!event_sys.initialized)
    {
        return 0;
    }

    int processed = 0;

    while (1)
    {
        k_mutex_lock(&event_sys.mutex, K_FOREVER);

        if (event_sys.queue_count == 0)
        {
            k_mutex_unlock(&event_sys.mutex);
            break;
        }

        /* Dequeue event */
        akira_event_t event = event_sys.queue[event_sys.queue_head];
        event_sys.queue_head = (event_sys.queue_head + 1) % AKIRA_EVENT_QUEUE_SIZE;
        event_sys.queue_count--;

        /* Deliver while holding lock (prevents subscription changes) */
        deliver_event(&event);

        k_mutex_unlock(&event_sys.mutex);
        processed++;
    }

    return processed;
}

int akira_event_wait(akira_event_type_t type, akira_event_t *event,
                     akira_duration_t timeout_ms)
{
    if (!event_sys.initialized)
    {
        return -1;
    }

    k_timeout_t timeout = (timeout_ms == AKIRA_WAIT_FOREVER) ? K_FOREVER : K_MSEC(timeout_ms);

    uint64_t start = k_uptime_get();

    while (1)
    {
        /* Wait for event in queue */
        if (k_sem_take(&event_sys.queue_sem, timeout) != 0)
        {
            return -1; /* Timeout */
        }

        k_mutex_lock(&event_sys.mutex, K_FOREVER);

        /* Search queue for matching event */
        for (int i = 0; i < event_sys.queue_count; i++)
        {
            int idx = (event_sys.queue_head + i) % AKIRA_EVENT_QUEUE_SIZE;
            if (event_sys.queue[idx].type == type)
            {
                if (event)
                {
                    *event = event_sys.queue[idx];
                }

                /* Remove from queue (shift remaining) */
                for (int j = i; j < event_sys.queue_count - 1; j++)
                {
                    int curr = (event_sys.queue_head + j) % AKIRA_EVENT_QUEUE_SIZE;
                    int next = (event_sys.queue_head + j + 1) % AKIRA_EVENT_QUEUE_SIZE;
                    event_sys.queue[curr] = event_sys.queue[next];
                }
                event_sys.queue_count--;

                k_mutex_unlock(&event_sys.mutex);
                return 0;
            }
        }

        k_mutex_unlock(&event_sys.mutex);

        /* Update timeout */
        if (timeout_ms != AKIRA_WAIT_FOREVER)
        {
            uint64_t elapsed = k_uptime_get() - start;
            if (elapsed >= timeout_ms)
            {
                return -1;
            }
            timeout = K_MSEC(timeout_ms - elapsed);
        }
    }
}

int akira_event_pending_count(void)
{
    return event_sys.queue_count;
}

void akira_event_clear_queue(void)
{
    k_mutex_lock(&event_sys.mutex, K_FOREVER);

    event_sys.queue_head = 0;
    event_sys.queue_tail = 0;
    event_sys.queue_count = 0;
    k_sem_reset(&event_sys.queue_sem);

    k_mutex_unlock(&event_sys.mutex);

    LOG_DBG("Cleared event queue");
}
