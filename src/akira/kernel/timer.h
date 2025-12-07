/**
 * @file timer.h
 * @brief AkiraOS Software Timer Interface
 *
 * Provides software timers, periodic callbacks, and time utilities
 * built on top of Zephyr's timer infrastructure.
 */

#ifndef AKIRA_KERNEL_TIMER_H
#define AKIRA_KERNEL_TIMER_H

#include "types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*===========================================================================*/
/* Constants                                                                 */
/*===========================================================================*/

/** Maximum number of software timers */
#define AKIRA_MAX_TIMERS 32

    /*===========================================================================*/
    /* Timer Types                                                               */
    /*===========================================================================*/

    /** Timer modes */
    typedef enum
    {
        AKIRA_TIMER_ONESHOT,  /**< Fire once then stop */
        AKIRA_TIMER_PERIODIC, /**< Fire repeatedly */
        AKIRA_TIMER_INTERVAL  /**< Fire at specific intervals */
    } akira_timer_mode_t;

    /** Timer states */
    typedef enum
    {
        AKIRA_TIMER_STOPPED, /**< Timer is stopped */
        AKIRA_TIMER_RUNNING, /**< Timer is active */
        AKIRA_TIMER_EXPIRED, /**< Timer has expired (oneshot) */
        AKIRA_TIMER_PAUSED   /**< Timer is paused */
    } akira_timer_state_t;

    /*===========================================================================*/
    /* Timer Callback                                                            */
    /*===========================================================================*/

    /** Forward declaration */
    typedef struct akira_timer akira_timer_t;

    /**
     * @brief Timer callback function
     * @param timer Timer that expired
     * @param user_data User-provided context
     */
    typedef void (*akira_timer_callback_t)(akira_timer_t *timer, void *user_data);

    /*===========================================================================*/
    /* Timer Structures                                                          */
    /*===========================================================================*/

    /** Timer configuration */
    typedef struct
    {
        const char *name;                /**< Timer name (optional) */
        akira_timer_mode_t mode;         /**< Timer mode */
        akira_duration_t period_ms;      /**< Period in milliseconds */
        akira_duration_t initial_ms;     /**< Initial delay (0 = use period) */
        akira_timer_callback_t callback; /**< Expiry callback */
        void *user_data;                 /**< User context */
        bool start_immediately;          /**< Start on creation */
    } akira_timer_config_t;

    /** Timer information */
    typedef struct
    {
        akira_handle_t id;             /**< Timer ID */
        const char *name;              /**< Timer name */
        akira_timer_mode_t mode;       /**< Timer mode */
        akira_timer_state_t state;     /**< Current state */
        akira_duration_t period_ms;    /**< Period in milliseconds */
        akira_duration_t remaining_ms; /**< Time until expiry */
        uint32_t fire_count;           /**< Number of times fired */
    } akira_timer_info_t;

    /*===========================================================================*/
    /* Timer API                                                                 */
    /*===========================================================================*/

    /**
     * @brief Initialize the timer subsystem
     * @return 0 on success, negative on error
     */
    int akira_timer_subsystem_init(void);

    /**
     * @brief Create a new timer
     * @param config Timer configuration
     * @return Timer handle or NULL on error
     */
    akira_timer_t *akira_timer_create(const akira_timer_config_t *config);

    /**
     * @brief Destroy a timer
     * @param timer Timer to destroy
     */
    void akira_timer_destroy(akira_timer_t *timer);

    /**
     * @brief Start a timer
     * @param timer Timer to start
     * @return 0 on success, negative on error
     */
    int akira_timer_start(akira_timer_t *timer);

    /**
     * @brief Stop a timer
     * @param timer Timer to stop
     * @return 0 on success, negative on error
     */
    int akira_timer_stop(akira_timer_t *timer);

    /**
     * @brief Reset a timer (restart from beginning)
     * @param timer Timer to reset
     * @return 0 on success, negative on error
     */
    int akira_timer_reset(akira_timer_t *timer);

    /**
     * @brief Pause a timer (preserves remaining time)
     * @param timer Timer to pause
     * @return 0 on success, negative on error
     */
    int akira_timer_pause(akira_timer_t *timer);

    /**
     * @brief Resume a paused timer
     * @param timer Timer to resume
     * @return 0 on success, negative on error
     */
    int akira_timer_resume(akira_timer_t *timer);

    /**
     * @brief Get timer state
     * @param timer Timer to query
     * @return Current timer state
     */
    akira_timer_state_t akira_timer_get_state(akira_timer_t *timer);

    /**
     * @brief Get remaining time until expiry
     * @param timer Timer to query
     * @return Remaining time in milliseconds
     */
    akira_duration_t akira_timer_remaining(akira_timer_t *timer);

    /**
     * @brief Get timer information
     * @param timer Timer to query
     * @param info Output information structure
     * @return 0 on success
     */
    int akira_timer_get_info(akira_timer_t *timer, akira_timer_info_t *info);

    /**
     * @brief Change timer period
     * @param timer Timer to modify
     * @param period_ms New period in milliseconds
     * @return 0 on success
     */
    int akira_timer_set_period(akira_timer_t *timer, akira_duration_t period_ms);

    /**
     * @brief Change timer callback
     * @param timer Timer to modify
     * @param callback New callback function
     * @param user_data New user data
     * @return 0 on success
     */
    int akira_timer_set_callback(akira_timer_t *timer,
                                 akira_timer_callback_t callback,
                                 void *user_data);

    /*===========================================================================*/
    /* Convenience Functions                                                     */
    /*===========================================================================*/

    /**
     * @brief Create and start a oneshot timer
     * @param delay_ms Delay before firing
     * @param callback Callback function
     * @param user_data User context
     * @return Timer handle or NULL
     */
    akira_timer_t *akira_timer_oneshot(akira_duration_t delay_ms,
                                       akira_timer_callback_t callback,
                                       void *user_data);

    /**
     * @brief Create and start a periodic timer
     * @param period_ms Period between fires
     * @param callback Callback function
     * @param user_data User context
     * @return Timer handle or NULL
     */
    akira_timer_t *akira_timer_periodic(akira_duration_t period_ms,
                                        akira_timer_callback_t callback,
                                        void *user_data);

    /**
     * @brief Schedule a delayed function call
     * @param delay_ms Delay before call
     * @param func Function to call
     * @param arg Function argument
     * @return 0 on success
     */
    int akira_call_after(akira_duration_t delay_ms,
                         void (*func)(void *), void *arg);

    /*===========================================================================*/
    /* Time Utilities                                                            */
    /*===========================================================================*/

    /**
     * @brief Get system uptime in milliseconds
     * @return Uptime in milliseconds
     */
    akira_duration_t akira_uptime_ms(void);

    /**
     * @brief Get system uptime in seconds
     * @return Uptime in seconds
     */
    uint32_t akira_uptime_sec(void);

    /**
     * @brief Get high-resolution timestamp
     * @return Timestamp in microseconds
     */
    uint64_t akira_timestamp_us(void);

    /**
     * @brief Sleep for a duration
     * @param ms Milliseconds to sleep
     */
    void akira_sleep_ms(akira_duration_t ms);

    /**
     * @brief Busy-wait for a duration
     * @param us Microseconds to wait
     */
    void akira_delay_us(uint32_t us);

    /**
     * @brief Get system tick count
     * @return Current tick count
     */
    uint32_t akira_ticks(void);

    /**
     * @brief Convert milliseconds to ticks
     * @param ms Milliseconds
     * @return Ticks
     */
    uint32_t akira_ms_to_ticks(akira_duration_t ms);

    /**
     * @brief Convert ticks to milliseconds
     * @param ticks Ticks
     * @return Milliseconds
     */
    akira_duration_t akira_ticks_to_ms(uint32_t ticks);

    /*===========================================================================*/
    /* Timer Statistics                                                          */
    /*===========================================================================*/

    /**
     * @brief Get active timer count
     * @return Number of active timers
     */
    int akira_timer_count(void);

    /**
     * @brief Print timer status
     */
    void akira_timer_print_all(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_KERNEL_TIMER_H */
