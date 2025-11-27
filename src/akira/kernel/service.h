/**
 * @file service.h
 * @brief AkiraOS Service Manager
 *
 * Manages system services with lifecycle control.
 * Services are long-running background tasks that provide
 * functionality to the system and applications.
 */

#ifndef AKIRA_KERNEL_SERVICE_H
#define AKIRA_KERNEL_SERVICE_H

#include "types.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /*===========================================================================*/
    /* Service Priorities                                                        */
    /*===========================================================================*/

    typedef enum
    {
        SERVICE_PRIORITY_CRITICAL = 0, /**< Must start first (kernel services) */
        SERVICE_PRIORITY_HIGH = 1,     /**< High priority (drivers) */
        SERVICE_PRIORITY_NORMAL = 2,   /**< Normal priority (most services) */
        SERVICE_PRIORITY_LOW = 3,      /**< Low priority (optional services) */
        SERVICE_PRIORITY_IDLE = 4      /**< Background services */
    } akira_service_priority_t;

    /*===========================================================================*/
    /* Service States                                                            */
    /*===========================================================================*/

    typedef enum
    {
        SERVICE_STATE_UNREGISTERED = 0,
        SERVICE_STATE_REGISTERED,
        SERVICE_STATE_INITIALIZING,
        SERVICE_STATE_READY,
        SERVICE_STATE_STARTING,
        SERVICE_STATE_RUNNING,
        SERVICE_STATE_STOPPING,
        SERVICE_STATE_STOPPED,
        SERVICE_STATE_ERROR
    } akira_service_state_t;

    /*===========================================================================*/
    /* Service Callbacks                                                         */
    /*===========================================================================*/

    /**
     * @brief Service lifecycle callback
     * @return 0 on success, negative error code on failure
     */
    typedef int (*akira_service_fn_t)(void);

    /**
     * @brief Service status callback
     * @return Current health status (0=ok, negative=error)
     */
    typedef int (*akira_service_status_fn_t)(void);

    /*===========================================================================*/
    /* Service Definition                                                        */
    /*===========================================================================*/

    /**
     * @brief Service descriptor
     */
    typedef struct akira_service
    {
        const char *name;                  /**< Service name (unique) */
        akira_service_priority_t priority; /**< Startup priority */

        /* Lifecycle callbacks */
        akira_service_fn_t init;          /**< Called once during registration */
        akira_service_fn_t start;         /**< Called to start the service */
        akira_service_fn_t stop;          /**< Called to stop the service */
        akira_service_status_fn_t status; /**< Called to check service health */

        /* Dependencies */
        const char **depends_on; /**< NULL-terminated list of dependencies */

        /* Runtime state (managed by service manager) */
        akira_service_state_t state;
        akira_service_handle_t handle;
        uint32_t start_time;
        uint32_t restart_count;
    } akira_service_t;

    /*===========================================================================*/
    /* Service Manager API                                                       */
    /*===========================================================================*/

    /**
     * @brief Initialize the service manager
     * @return 0 on success
     */
    int akira_service_manager_init(void);

    /**
     * @brief Register a service
     * @param service Service descriptor
     * @return Service handle or negative error
     */
    akira_service_handle_t akira_service_register(akira_service_t *service);

    /**
     * @brief Unregister a service
     * @param handle Service handle
     * @return 0 on success
     */
    int akira_service_unregister(akira_service_handle_t handle);

    /**
     * @brief Start a service
     * @param name Service name
     * @return 0 on success
     */
    int akira_service_start(const char *name);

    /**
     * @brief Stop a service
     * @param name Service name
     * @return 0 on success
     */
    int akira_service_stop(const char *name);

    /**
     * @brief Restart a service
     * @param name Service name
     * @return 0 on success
     */
    int akira_service_restart(const char *name);

    /**
     * @brief Get service state
     * @param name Service name
     * @return Service state
     */
    akira_service_state_t akira_service_get_state(const char *name);

    /**
     * @brief Check if service is running
     * @param name Service name
     * @return true if running
     */
    bool akira_service_is_running(const char *name);

    /**
     * @brief Start all registered services
     * @return 0 on success
     */
    int akira_service_start_all(void);

    /**
     * @brief Stop all services
     * @return 0 on success
     */
    int akira_service_stop_all(void);

    /**
     * @brief Get service by name
     * @param name Service name
     * @return Service pointer or NULL
     */
    akira_service_t *akira_service_get(const char *name);

    /**
     * @brief Get service by handle
     * @param handle Service handle
     * @return Service pointer or NULL
     */
    akira_service_t *akira_service_get_by_handle(akira_service_handle_t handle);

    /**
     * @brief List all services
     * @param services Output array
     * @param max_count Maximum entries
     * @return Number of services
     */
    int akira_service_list(akira_service_t **services, int max_count);

    /**
     * @brief Print service status (debug)
     */
    void akira_service_print_status(void);

    /**
     * @brief Print all services (debug)
     */
    void akira_service_print_all(void);

    /**
     * @brief Get number of registered services
     * @return Service count
     */
    int akira_service_count(void);

    /**
     * @brief Find service by name
     * @param name Service name
     * @return Service pointer or NULL
     */
    akira_service_t *akira_service_find_by_name(const char *name);

/*===========================================================================*/
/* Service Registration Macro                                                */
/*===========================================================================*/

/**
 * @brief Declare a service for automatic registration
 *
 * Usage:
 *   AKIRA_SERVICE_DEFINE(my_service, "my_service", SERVICE_PRIORITY_NORMAL,
 *                        my_init, my_start, my_stop, my_status, NULL);
 */
#define AKIRA_SERVICE_DEFINE(var_name, name, priority, init, start, stop, status, deps) \
    static akira_service_t var_name = {                                                 \
        .name = name,                                                                   \
        .priority = priority,                                                           \
        .init = init,                                                                   \
        .start = start,                                                                 \
        .stop = stop,                                                                   \
        .status = status,                                                               \
        .depends_on = deps,                                                             \
        .state = SERVICE_STATE_UNREGISTERED,                                            \
        .handle = AKIRA_INVALID_HANDLE,                                                 \
        .start_time = 0,                                                                \
        .restart_count = 0}

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_KERNEL_SERVICE_H */
