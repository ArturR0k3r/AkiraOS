/**
 * @file service_manager.c
 * @brief AkiraOS System Service Manager Implementation
 */

#include "service_manager.h"
#include <string.h>

#define MAX_SERVICES 16
static const akira_service_t *services[MAX_SERVICES];
static int service_count = 0;

int service_manager_register(const akira_service_t *service)
{
    if (!service || !service->name)
        return -1;
    if (service_count >= MAX_SERVICES)
        return -2;
    for (int i = 0; i < service_count; ++i)
    {
        if (strcmp(services[i]->name, service->name) == 0)
            return -3;
    }
    services[service_count++] = service;
    return 0;
}

int service_manager_start(const char *name)
{
    for (int i = 0; i < service_count; ++i)
    {
        if (strcmp(services[i]->name, name) == 0 && services[i]->start)
        {
            services[i]->start();
            ((akira_service_t *)services[i])->running = true;
            return 0;
        }
    }
    return -1;
}

int service_manager_stop(const char *name)
{
    for (int i = 0; i < service_count; ++i)
    {
        if (strcmp(services[i]->name, name) == 0 && services[i]->stop)
        {
            services[i]->stop();
            ((akira_service_t *)services[i])->running = false;
            return 0;
        }
    }
    return -1;
}

int service_manager_status(const char *name)
{
    for (int i = 0; i < service_count; ++i)
    {
        if (strcmp(services[i]->name, name) == 0)
        {
            return services[i]->running ? 1 : 0;
        }
    }
    return -1;
}
