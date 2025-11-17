
#ifndef AKIRA_SERVICE_MANAGER_H
#define AKIRA_SERVICE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

typedef int (*service_init_fn)(void);
typedef int (*service_start_fn)(void);
typedef int (*service_stop_fn)(void);
typedef int (*service_status_fn)(void);

typedef struct akira_service
{
    const char *name;
    service_init_fn init;
    service_start_fn start;
    service_stop_fn stop;
    service_status_fn status;
    bool running;
} akira_service_t;

int service_manager_register(const akira_service_t *service);
int service_manager_start(const char *name);
int service_manager_stop(const char *name);
int service_manager_status(const char *name);

#endif // AKIRA_SERVICE_MANAGER_H
