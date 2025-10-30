/**
 * @file event_system.c
 * @brief AkiraOS Event System Implementation
 */

#include "event_system.h"
#include <string.h>
#include <zephyr/kernel.h>

#define MAX_EVENT_HANDLERS 16
#define EVENT_QUEUE_SIZE 32

typedef struct
{
    akira_event_type_t type;
    akira_event_handler_t handler;
} event_handler_entry_t;

static event_handler_entry_t handlers[MAX_EVENT_HANDLERS];
static int handler_count = 0;

static akira_event_t event_queue[EVENT_QUEUE_SIZE];
static int event_queue_head = 0;
static int event_queue_tail = 0;
static struct k_mutex event_mutex;

static void event_queue_init(void)
{
    k_mutex_init(&event_mutex);
    event_queue_head = event_queue_tail = 0;
}

int event_system_publish(const akira_event_t *event)
{
    if (!event)
        return -1;
    k_mutex_lock(&event_mutex, K_FOREVER);
    int next_tail = (event_queue_tail + 1) % EVENT_QUEUE_SIZE;
    if (next_tail == event_queue_head)
    {
        k_mutex_unlock(&event_mutex);
        return -2; // Queue full
    }
    event_queue[event_queue_tail] = *event;
    event_queue_tail = next_tail;
    k_mutex_unlock(&event_mutex);
    return 0;
}

int event_system_poll(void)
{
    k_mutex_lock(&event_mutex, K_FOREVER);
    if (event_queue_head == event_queue_tail)
    {
        k_mutex_unlock(&event_mutex);
        return 0; // No events
    }
    akira_event_t event = event_queue[event_queue_head];
    event_queue_head = (event_queue_head + 1) % EVENT_QUEUE_SIZE;
    k_mutex_unlock(&event_mutex);
    for (int i = 0; i < handler_count; ++i)
    {
        if (handlers[i].type == event.type && handlers[i].handler)
        {
            handlers[i].handler(&event);
        }
    }
    return 1;
}

int event_system_subscribe(akira_event_type_t type, akira_event_handler_t handler)
{
    if (!handler || handler_count >= MAX_EVENT_HANDLERS)
        return -1;
    handlers[handler_count].type = type;
    handlers[handler_count].handler = handler;
    handler_count++;
    return 0;
}

int event_system_unsubscribe(akira_event_type_t type, akira_event_handler_t handler)
{
    for (int i = 0; i < handler_count; ++i)
    {
        if (handlers[i].type == type && handlers[i].handler == handler)
        {
            for (int j = i; j < handler_count - 1; ++j)
            {
                handlers[j] = handlers[j + 1];
            }
            handler_count--;
            return 0;
        }
    }
    return -1;
}

// Call event_queue_init() during system startup (akiraos_init)
