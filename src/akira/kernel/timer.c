/**
 * @file timer.c
 * @brief AkiraOS Software Timer Implementation
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "timer.h"
#include "memory.h"

LOG_MODULE_REGISTER(akira_timer, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Internal Structures                                                       */
/*===========================================================================*/

struct akira_timer
{
    bool in_use;
    char name[32];
    akira_handle_t id;
    akira_timer_mode_t mode;
    akira_timer_state_t state;
    akira_duration_t period_ms;
    akira_duration_t initial_ms;
    akira_duration_t remaining_ms;
    akira_timer_callback_t callback;
    void *user_data;
    uint32_t fire_count;

    struct k_timer k_timer;
};

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    akira_timer_t timers[AKIRA_MAX_TIMERS];
    akira_handle_t next_id;
    int active_count;
    struct k_mutex mutex;
} timer_mgr;

/*===========================================================================*/
/* Internal Functions                                                        */
/*===========================================================================*/

static akira_timer_t *find_timer_by_k_timer(struct k_timer *k_timer)
{
    for (int i = 0; i < AKIRA_MAX_TIMERS; i++)
    {
        if (timer_mgr.timers[i].in_use &&
            &timer_mgr.timers[i].k_timer == k_timer)
        {
            return &timer_mgr.timers[i];
        }
    }
    return NULL;
}

static akira_timer_t *find_free_slot(void)
{
    for (int i = 0; i < AKIRA_MAX_TIMERS; i++)
    {
        if (!timer_mgr.timers[i].in_use)
        {
            return &timer_mgr.timers[i];
        }
    }
    return NULL;
}

static void timer_expiry_handler(struct k_timer *k_timer)
{
    akira_timer_t *timer = find_timer_by_k_timer(k_timer);
    if (!timer)
    {
        return;
    }

    timer->fire_count++;

    if (timer->mode == AKIRA_TIMER_ONESHOT)
    {
        timer->state = AKIRA_TIMER_EXPIRED;
    }

    if (timer->callback)
    {
        timer->callback(timer, timer->user_data);
    }
}

static void timer_stop_handler(struct k_timer *k_timer)
{
    akira_timer_t *timer = find_timer_by_k_timer(k_timer);
    if (timer)
    {
        timer->state = AKIRA_TIMER_STOPPED;
    }
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int akira_timer_subsystem_init(void)
{
    if (timer_mgr.initialized)
    {
        return 0;
    }

    LOG_INF("Initializing timer subsystem");

    k_mutex_init(&timer_mgr.mutex);
    memset(&timer_mgr.timers, 0, sizeof(timer_mgr.timers));
    timer_mgr.next_id = 1;
    timer_mgr.active_count = 0;

    timer_mgr.initialized = true;

    LOG_INF("Timer subsystem initialized (max=%d)", AKIRA_MAX_TIMERS);

    return 0;
}

akira_timer_t *akira_timer_create(const akira_timer_config_t *config)
{
    if (!timer_mgr.initialized || !config)
    {
        return NULL;
    }

    k_mutex_lock(&timer_mgr.mutex, K_FOREVER);

    akira_timer_t *timer = find_free_slot();
    if (!timer)
    {
        k_mutex_unlock(&timer_mgr.mutex);
        LOG_ERR("No free timer slots");
        return NULL;
    }

    memset(timer, 0, sizeof(*timer));

    timer->in_use = true;
    timer->id = timer_mgr.next_id++;
    timer->mode = config->mode;
    timer->state = AKIRA_TIMER_STOPPED;
    timer->period_ms = config->period_ms;
    timer->initial_ms = config->initial_ms > 0 ? config->initial_ms : config->period_ms;
    timer->callback = config->callback;
    timer->user_data = config->user_data;

    if (config->name)
    {
        strncpy(timer->name, config->name, sizeof(timer->name) - 1);
    }
    else
    {
        snprintf(timer->name, sizeof(timer->name), "timer_%u", timer->id);
    }

    k_timer_init(&timer->k_timer, timer_expiry_handler, timer_stop_handler);

    timer_mgr.active_count++;

    k_mutex_unlock(&timer_mgr.mutex);

    LOG_DBG("Created timer '%s' (id=%u, mode=%d, period=%ums)",
            timer->name, timer->id, timer->mode, timer->period_ms);

    if (config->start_immediately)
    {
        akira_timer_start(timer);
    }

    return timer;
}

void akira_timer_destroy(akira_timer_t *timer)
{
    if (!timer || !timer->in_use)
    {
        return;
    }

    k_mutex_lock(&timer_mgr.mutex, K_FOREVER);

    LOG_DBG("Destroying timer '%s'", timer->name);

    k_timer_stop(&timer->k_timer);

    timer->in_use = false;
    timer_mgr.active_count--;

    k_mutex_unlock(&timer_mgr.mutex);
}

int akira_timer_start(akira_timer_t *timer)
{
    if (!timer || !timer->in_use)
    {
        return -1;
    }

    k_timeout_t initial = K_MSEC(timer->initial_ms);
    k_timeout_t period;

    switch (timer->mode)
    {
    case AKIRA_TIMER_ONESHOT:
        period = K_NO_WAIT;
        break;
    case AKIRA_TIMER_PERIODIC:
    case AKIRA_TIMER_INTERVAL:
        period = K_MSEC(timer->period_ms);
        break;
    default:
        return -1;
    }

    k_timer_start(&timer->k_timer, initial, period);
    timer->state = AKIRA_TIMER_RUNNING;

    LOG_DBG("Started timer '%s'", timer->name);

    return 0;
}

int akira_timer_stop(akira_timer_t *timer)
{
    if (!timer || !timer->in_use)
    {
        return -1;
    }

    k_timer_stop(&timer->k_timer);
    timer->state = AKIRA_TIMER_STOPPED;

    LOG_DBG("Stopped timer '%s'", timer->name);

    return 0;
}

int akira_timer_reset(akira_timer_t *timer)
{
    if (!timer || !timer->in_use)
    {
        return -1;
    }

    akira_timer_stop(timer);
    timer->fire_count = 0;
    return akira_timer_start(timer);
}

int akira_timer_pause(akira_timer_t *timer)
{
    if (!timer || !timer->in_use ||
        timer->state != AKIRA_TIMER_RUNNING)
    {
        return -1;
    }

    timer->remaining_ms = akira_timer_remaining(timer);
    k_timer_stop(&timer->k_timer);
    timer->state = AKIRA_TIMER_PAUSED;

    return 0;
}

int akira_timer_resume(akira_timer_t *timer)
{
    if (!timer || !timer->in_use ||
        timer->state != AKIRA_TIMER_PAUSED)
    {
        return -1;
    }

    k_timeout_t remaining = K_MSEC(timer->remaining_ms);
    k_timeout_t period = (timer->mode == AKIRA_TIMER_ONESHOT) ? K_NO_WAIT : K_MSEC(timer->period_ms);

    k_timer_start(&timer->k_timer, remaining, period);
    timer->state = AKIRA_TIMER_RUNNING;

    return 0;
}

akira_timer_state_t akira_timer_get_state(akira_timer_t *timer)
{
    return timer ? timer->state : AKIRA_TIMER_STOPPED;
}

akira_duration_t akira_timer_remaining(akira_timer_t *timer)
{
    if (!timer || !timer->in_use)
    {
        return 0;
    }

    if (timer->state == AKIRA_TIMER_PAUSED)
    {
        return timer->remaining_ms;
    }

    return k_timer_remaining_get(&timer->k_timer);
}

int akira_timer_get_info(akira_timer_t *timer, akira_timer_info_t *info)
{
    if (!timer || !info)
    {
        return -1;
    }

    info->id = timer->id;
    info->name = timer->name;
    info->mode = timer->mode;
    info->state = timer->state;
    info->period_ms = timer->period_ms;
    info->remaining_ms = akira_timer_remaining(timer);
    info->fire_count = timer->fire_count;

    return 0;
}

int akira_timer_set_period(akira_timer_t *timer, akira_duration_t period_ms)
{
    if (!timer)
        return -1;

    timer->period_ms = period_ms;

    if (timer->state == AKIRA_TIMER_RUNNING)
    {
        akira_timer_reset(timer);
    }

    return 0;
}

int akira_timer_set_callback(akira_timer_t *timer,
                             akira_timer_callback_t callback,
                             void *user_data)
{
    if (!timer)
        return -1;

    timer->callback = callback;
    timer->user_data = user_data;

    return 0;
}

/*===========================================================================*/
/* Convenience Functions                                                     */
/*===========================================================================*/

akira_timer_t *akira_timer_oneshot(akira_duration_t delay_ms,
                                   akira_timer_callback_t callback,
                                   void *user_data)
{
    akira_timer_config_t config = {
        .mode = AKIRA_TIMER_ONESHOT,
        .period_ms = delay_ms,
        .callback = callback,
        .user_data = user_data,
        .start_immediately = true};
    return akira_timer_create(&config);
}

akira_timer_t *akira_timer_periodic(akira_duration_t period_ms,
                                    akira_timer_callback_t callback,
                                    void *user_data)
{
    akira_timer_config_t config = {
        .mode = AKIRA_TIMER_PERIODIC,
        .period_ms = period_ms,
        .callback = callback,
        .user_data = user_data,
        .start_immediately = true};
    return akira_timer_create(&config);
}

/* Simple delayed call structure */
struct delayed_call
{
    void (*func)(void *);
    void *arg;
};

static void delayed_call_handler(akira_timer_t *timer, void *user_data)
{
    struct delayed_call *call = (struct delayed_call *)user_data;
    if (call && call->func)
    {
        call->func(call->arg);
    }
    akira_free(call);
    akira_timer_destroy(timer);
}

int akira_call_after(akira_duration_t delay_ms,
                     void (*func)(void *), void *arg)
{
    struct delayed_call *call = akira_malloc(sizeof(struct delayed_call));
    if (!call)
        return -1;

    call->func = func;
    call->arg = arg;

    akira_timer_t *timer = akira_timer_oneshot(delay_ms,
                                               delayed_call_handler, call);
    if (!timer)
    {
        akira_free(call);
        return -1;
    }

    return 0;
}

/*===========================================================================*/
/* Time Utilities                                                            */
/*===========================================================================*/

akira_duration_t akira_uptime_ms(void)
{
    return k_uptime_get();
}

uint32_t akira_uptime_sec(void)
{
    return k_uptime_get() / 1000;
}

uint64_t akira_timestamp_us(void)
{
    return k_ticks_to_us_floor64(k_uptime_ticks());
}

void akira_sleep_ms(akira_duration_t ms)
{
    k_msleep(ms);
}

void akira_delay_us(uint32_t us)
{
    k_busy_wait(us);
}

uint32_t akira_ticks(void)
{
    return k_uptime_ticks();
}

uint32_t akira_ms_to_ticks(akira_duration_t ms)
{
    return k_ms_to_ticks_ceil32(ms);
}

akira_duration_t akira_ticks_to_ms(uint32_t ticks)
{
    return k_ticks_to_ms_floor32(ticks);
}

/*===========================================================================*/
/* Timer Statistics                                                          */
/*===========================================================================*/

int akira_timer_count(void)
{
    return timer_mgr.active_count;
}

void akira_timer_print_all(void)
{
    LOG_INF("=== Timer Status ===");
    LOG_INF("Active timers: %d/%d", timer_mgr.active_count, AKIRA_MAX_TIMERS);

    static const char *state_names[] = {
        "STOPPED", "RUNNING", "EXPIRED", "PAUSED"};

    static const char *mode_names[] = {
        "ONESHOT", "PERIODIC", "INTERVAL"};

    for (int i = 0; i < AKIRA_MAX_TIMERS; i++)
    {
        akira_timer_t *t = &timer_mgr.timers[i];
        if (t->in_use)
        {
            LOG_INF("  %s: %s %s period=%ums remaining=%ums fired=%u",
                    t->name,
                    mode_names[t->mode],
                    state_names[t->state],
                    t->period_ms,
                    akira_timer_remaining(t),
                    t->fire_count);
        }
    }
}