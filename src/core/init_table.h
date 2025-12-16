/**
 * @file init_table.h
 * @brief Subsystem Initialization Table for AkiraOS
 * 
 * Provides a priority-based initialization system where subsystems register
 * themselves and are initialized in the correct order.
 */

#ifndef AKIRA_INIT_TABLE_H
#define AKIRA_INIT_TABLE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialization priorities
 */
typedef enum {
    INIT_PRIORITY_EARLY = 0,      /* HAL, event bus, core systems */
    INIT_PRIORITY_PLATFORM = 10,   /* Driver registry, hardware managers */
    INIT_PRIORITY_DRIVERS = 20,    /* Device drivers */
    INIT_PRIORITY_STORAGE = 30,    /* Filesystems, settings */
    INIT_PRIORITY_NETWORK = 40,    /* WiFi, Bluetooth, USB */
    INIT_PRIORITY_SERVICES = 50,   /* App manager, OCRE, OTA */
    INIT_PRIORITY_APPS = 60,       /* User applications */
    INIT_PRIORITY_LATE = 70        /* Shell, final setup */
} init_priority_t;

/**
 * @brief Subsystem initialization function
 * 
 * @return 0 on success, negative errno on failure
 */
typedef int (*subsystem_init_fn)(void);

/**
 * @brief Subsystem entry in initialization table
 */
typedef struct {
    const char *name;
    init_priority_t priority;
    subsystem_init_fn init_fn;
    bool required;  /* System fails if initialization fails */
    bool enabled;   /* Controlled by Kconfig */
} subsystem_entry_t;

/**
 * @brief Register a subsystem for initialization
 * 
 * @param name Subsystem name (for logging)
 * @param priority Initialization priority
 * @param init_fn Initialization function
 * @param required True if system should fail if init fails
 * @param enabled True if subsystem is enabled (from Kconfig)
 * @return 0 on success, negative errno on failure
 */
int init_table_register(const char *name, 
                       init_priority_t priority,
                       subsystem_init_fn init_fn,
                       bool required,
                       bool enabled);

/**
 * @brief Initialize all registered subsystems in priority order
 * 
 * @return 0 on success, negative errno on failure
 */
int init_table_run(void);

/**
 * @brief Get initialization statistics
 * 
 * @param total_count Total subsystems registered
 * @param success_count Successfully initialized
 * @param failed_count Failed initialization
 */
void init_table_get_stats(int *total_count, int *success_count, int *failed_count);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_INIT_TABLE_H */
