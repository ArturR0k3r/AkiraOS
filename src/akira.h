/**
 * @file akira.h
 * @brief AkiraOS Core Header
 *
 * Central include file for AkiraOS core functionality.
 * Include this file to access all core OS features.
 *
 * @author Artur R0K3R
 * @version 1.4.8
 */

#ifndef AKIRA_CORE_H
#define AKIRA_CORE_H

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*===========================================================================*/
    /* Version Information                                                       */
    /*===========================================================================*/

#define AKIRA_VERSION_MAJOR 1
#define AKIRA_VERSION_MINOR 4
#define AKIRA_VERSION_PATCH 8
#define AKIRA_VERSION_STRING "1.4.x"
#define AKIRA_CODENAME "GL1TCH"

    /* Simple version struct */
    typedef struct {
        int major;
        int minor;
        int patch;
        const char *codename;
    } akira_version_t;

    /*===========================================================================*/
    /* Logging Configuration                                                     */
    /*===========================================================================*/

#ifndef AKIRA_LOG_LEVEL
#define AKIRA_LOG_LEVEL LOG_LEVEL_INF
#endif

    /*===========================================================================*/
    /* System Limits                                                             */
    /*===========================================================================*/

#define AKIRA_MAX_SERVICES 16
#define AKIRA_MAX_PROCESSES 16
#define AKIRA_MAX_APPS 8
#define AKIRA_MAX_EVENTS 32
#define AKIRA_MAX_NAME_LEN 32

    /*===========================================================================*/
    /* Error Codes                                                               */
    /*===========================================================================*/

#define AKIRA_OK 0
#define AKIRA_ERR_INVALID (-1)
#define AKIRA_ERR_NOMEM (-2)
#define AKIRA_ERR_EXISTS (-3)
#define AKIRA_ERR_NOT_FOUND (-4)
#define AKIRA_ERR_BUSY (-5)
#define AKIRA_ERR_TIMEOUT (-6)
#define AKIRA_ERR_PERMISSION (-7)
#define AKIRA_ERR_NOT_READY (-8)
#define AKIRA_ERR_IO (-9)
#define AKIRA_ERR_UNSUPPORTED (-10)

    /*===========================================================================*/
    /* Forward Declarations                                                      */
    /*===========================================================================*/

    /* Core types (defined in respective headers) */
    struct akira_service;
    struct akira_event;
    struct akira_process;
    struct akira_app;

/*===========================================================================*/
/* Core Module Headers                                                       */
/*===========================================================================*/

/* Legacy kernel headers are intentionally omitted in Minimalist build; HAL is provided via drivers/platform_hal.h */
#include "drivers/platform_hal.h"

    /*===========================================================================*/
    /* System Initialization                                                     */
    /*===========================================================================*/

    /**
     * @brief Initialize AkiraOS core
     *
     * Must be called before any other AkiraOS function.
     * Initializes all core subsystems in the correct order.
     *
     * @return AKIRA_OK on success, error code on failure
     */
    int akira_init(void);

    /**
     * @brief Start AkiraOS
     *
     * Starts all registered services and enters the main loop.
     * This function does not return under normal operation.
     *
     * @return AKIRA_OK if shutdown was requested, error code on failure
     */
    int akira_start(void);

    /**
     * @brief Request AkiraOS shutdown
     *
     * Signals all services to stop and prepares for shutdown.
     *
     * @param reason Shutdown reason string (for logging)
     */
    void akira_shutdown(const char *reason);

    /* Note: akira_uptime_ms() is declared in kernel/timer.h */

    /**
     * @brief Get AkiraOS version string
     *
     * Kept for compatibility with older callers; prefer
     * `akira_version_string()` which is the canonical implementation.
     *
     * @return Version string (e.g., "1.2.3")
     */
    const char *akira_version(void);

    /**
     * @brief Get AkiraOS version string (canonical)
     *
     * @return Version string (e.g., "1.2.3")
     */
    const char *akira_version_string(void);

    /**
     * @brief Print the Akira banner to console
     */
    void akira_print_banner(void);

    /**
     * @brief Print a compact Akira status summary (logs)
     */
    void akira_print_status(void);

    /**
     * @brief Check if system is fully initialized
     *
     * Kept for compatibility; prefer `akira_is_initialized()`.
     *
     * @return true if system is ready
     */
    bool akira_is_ready(void);

    /**
     * @brief Check if system is initialized (canonical)
     *
     * @return true if system is initialized
     */
    bool akira_is_initialized(void);

    /**
     * @brief Check if system is currently running
     *
     * @return true if Akira is running
     */
    bool akira_is_running(void);

    /**
     * @brief Retrieve version struct
     */
    void akira_version_get(akira_version_t *version);

    /**
     * @brief Get initialization time in ms
     */
    uint32_t akira_init_time(void);

    /*===========================================================================*/
    /* System State                                                              */
    /*===========================================================================*/

    /**
     * @brief System state enum
     */
    typedef enum
    {
        AKIRA_STATE_UNINITIALIZED = 0,
        AKIRA_STATE_INITIALIZING,
        AKIRA_STATE_READY,
        AKIRA_STATE_RUNNING,
        AKIRA_STATE_STOPPING,
        AKIRA_STATE_STOPPED,
        AKIRA_STATE_ERROR
    } akira_state_t;

    /**
     * @brief Get current system state
     *
     * @return Current system state
     */
    akira_state_t akira_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CORE_H */
