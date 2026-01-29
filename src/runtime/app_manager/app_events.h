/**
 * @file app_events.h
 * @brief Application Event System
 * 
 * Breaks circular dependency between app_manager and akira_runtime
 * by using event-driven architecture.
 */

#ifndef AKIRA_APP_EVENTS_H
#define AKIRA_APP_EVENTS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Application event types */
typedef enum {
    APP_EVENT_NONE = 0,
    
    /* Lifecycle events */
    APP_EVENT_LOADING,       /* App binary loaded into memory */
    APP_EVENT_LOADED,        /* App ready to start */
    APP_EVENT_STARTING,      /* App initialization beginning */
    APP_EVENT_STARTED,       /* App successfully started */
    APP_EVENT_STOPPING,      /* App shutdown initiated */
    APP_EVENT_STOPPED,       /* App stopped normally */
    
    /* Error events */
    APP_EVENT_CRASHED,       /* App crashed/trapped */
    APP_EVENT_KILLED,        /* App forcefully terminated */
    APP_EVENT_TIMEOUT,       /* App exceeded execution timeout */
    APP_EVENT_OOM,           /* App out of memory */
    
    /* Resource events */
    APP_EVENT_MEMORY_WARN,   /* Memory usage warning */
    APP_EVENT_CPU_WARN,      /* CPU usage warning */
    APP_EVENT_STORAGE_WARN,  /* Storage quota warning */
    
    APP_EVENT_MAX
} app_event_type_t;

/* Application event data */
typedef struct {
    app_event_type_t type;
    int container_id;        /* Runtime container ID */
    char app_name[64];       /* Application name */
    uint32_t timestamp_ms;   /* Event timestamp */
    
    union {
        struct {
            int exit_code;
            const char *reason;
        } stopped;
        
        struct {
            int signal;
            void *fault_addr;
            const char *backtrace;
        } crashed;
        
        struct {
            uint32_t used;
            uint32_t limit;
            uint8_t percentage;
        } resource_warning;
    } data;
} app_event_t;

/* Event handler callback */
typedef void (*app_event_handler_t)(const app_event_t *event, void *user_data);

/**
 * @brief Initialize app event system
 * @return 0 on success
 */
int app_events_init(void);

/**
 * @brief Register event handler
 * @param handler Callback function
 * @param user_data User data passed to callback
 * @return Handler ID, or negative on error
 */
int app_events_register_handler(app_event_handler_t handler, void *user_data);

/**
 * @brief Unregister event handler
 * @param handler_id Handler ID returned from registration
 * @return 0 on success
 */
int app_events_unregister_handler(int handler_id);

/**
 * @brief Publish application event
 * @param event Event to publish
 * @return 0 on success
 */
int app_events_publish(const app_event_t *event);

/**
 * @brief Publish simple event (convenience function)
 * @param type Event type
 * @param container_id Container ID
 * @param app_name Application name
 * @return 0 on success
 */
int app_events_publish_simple(app_event_type_t type, int container_id, 
                              const char *app_name);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_APP_EVENTS_H */
