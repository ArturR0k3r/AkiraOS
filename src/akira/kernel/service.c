/**
 * @file service.c
 * @brief AkiraOS Service Manager Implementation
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "service.h"

LOG_MODULE_REGISTER(akira_service, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

#ifndef AKIRA_MAX_SERVICES
#define AKIRA_MAX_SERVICES 16
#endif

/*===========================================================================*/
/* Internal State                                                            */
/*===========================================================================*/

static struct
{
    bool initialized;
    akira_service_t *services[AKIRA_MAX_SERVICES];
    int count;
    struct k_mutex mutex;
} service_mgr;

/*===========================================================================*/
/* Internal Functions                                                        */
/*===========================================================================*/

/**
 * @brief Find service by name
 */
static akira_service_t *find_service(const char *name)
{
    if (!name)
        return NULL;

    for (int i = 0; i < service_mgr.count; i++)
    {
        if (service_mgr.services[i] &&
            strcmp(service_mgr.services[i]->name, name) == 0)
        {
            return service_mgr.services[i];
        }
    }
    return NULL;
}

/**
 * @brief Check if all dependencies are running
 */
static bool check_dependencies(akira_service_t *service)
{
    if (!service->depends_on)
    {
        return true;
    }

    for (const char **dep = service->depends_on; *dep != NULL; dep++)
    {
        akira_service_t *dep_svc = find_service(*dep);
        if (!dep_svc || dep_svc->state != SERVICE_STATE_RUNNING)
        {
            LOG_WRN("Service '%s' waiting for dependency '%s'",
                    service->name, *dep);
            return false;
        }
    }
    return true;
}

/**
 * @brief Compare services by priority for sorting
 */
static int compare_priority(const void *a, const void *b)
{
    const akira_service_t *sa = *(const akira_service_t **)a;
    const akira_service_t *sb = *(const akira_service_t **)b;
    return (int)sa->priority - (int)sb->priority;
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int akira_service_manager_init(void)
{
    if (service_mgr.initialized)
    {
        return 0;
    }

    LOG_INF("Initializing service manager");

    k_mutex_init(&service_mgr.mutex);

    for (int i = 0; i < AKIRA_MAX_SERVICES; i++)
    {
        service_mgr.services[i] = NULL;
    }
    service_mgr.count = 0;
    service_mgr.initialized = true;

    return 0;
}

akira_service_handle_t akira_service_register(akira_service_t *service)
{
    if (!service_mgr.initialized || !service || !service->name)
    {
        return AKIRA_INVALID_HANDLE;
    }

    k_mutex_lock(&service_mgr.mutex, K_FOREVER);

    /* Check for duplicate */
    if (find_service(service->name))
    {
        k_mutex_unlock(&service_mgr.mutex);
        LOG_ERR("Service '%s' already registered", service->name);
        return AKIRA_INVALID_HANDLE;
    }

    /* Find free slot */
    if (service_mgr.count >= AKIRA_MAX_SERVICES)
    {
        k_mutex_unlock(&service_mgr.mutex);
        LOG_ERR("Maximum services reached");
        return AKIRA_INVALID_HANDLE;
    }

    /* Register service */
    akira_service_handle_t handle = service_mgr.count;
    service->handle = handle;
    service->state = SERVICE_STATE_REGISTERED;
    service->start_time = 0;
    service->restart_count = 0;

    service_mgr.services[service_mgr.count++] = service;

    /* Call init if provided */
    if (service->init)
    {
        service->state = SERVICE_STATE_INITIALIZING;
        int ret = service->init();
        if (ret != 0)
        {
            LOG_ERR("Service '%s' init failed: %d", service->name, ret);
            service->state = SERVICE_STATE_ERROR;
            k_mutex_unlock(&service_mgr.mutex);
            return AKIRA_INVALID_HANDLE;
        }
    }

    service->state = SERVICE_STATE_READY;

    k_mutex_unlock(&service_mgr.mutex);

    LOG_INF("Registered service '%s' (handle=%d, priority=%d)",
            service->name, handle, service->priority);

    return handle;
}

int akira_service_unregister(akira_service_handle_t handle)
{
    if (!service_mgr.initialized || !AKIRA_HANDLE_VALID(handle))
    {
        return -1;
    }

    k_mutex_lock(&service_mgr.mutex, K_FOREVER);

    akira_service_t *service = akira_service_get_by_handle(handle);
    if (!service)
    {
        k_mutex_unlock(&service_mgr.mutex);
        return -1;
    }

    /* Stop if running */
    if (service->state == SERVICE_STATE_RUNNING)
    {
        if (service->stop)
        {
            service->stop();
        }
    }

    LOG_INF("Unregistered service '%s'", service->name);

    /* Remove from array */
    for (int i = 0; i < service_mgr.count; i++)
    {
        if (service_mgr.services[i] == service)
        {
            /* Shift remaining services */
            for (int j = i; j < service_mgr.count - 1; j++)
            {
                service_mgr.services[j] = service_mgr.services[j + 1];
                service_mgr.services[j]->handle = j;
            }
            service_mgr.count--;
            break;
        }
    }

    service->state = SERVICE_STATE_UNREGISTERED;
    service->handle = AKIRA_INVALID_HANDLE;

    k_mutex_unlock(&service_mgr.mutex);
    return 0;
}

int akira_service_start(const char *name)
{
    if (!service_mgr.initialized || !name)
    {
        return -1;
    }

    k_mutex_lock(&service_mgr.mutex, K_FOREVER);

    akira_service_t *service = find_service(name);
    if (!service)
    {
        k_mutex_unlock(&service_mgr.mutex);
        LOG_ERR("Service '%s' not found", name);
        return -1;
    }

    if (service->state == SERVICE_STATE_RUNNING)
    {
        k_mutex_unlock(&service_mgr.mutex);
        return 0; /* Already running */
    }

    if (service->state != SERVICE_STATE_READY &&
        service->state != SERVICE_STATE_STOPPED)
    {
        k_mutex_unlock(&service_mgr.mutex);
        LOG_ERR("Service '%s' not in startable state (%d)",
                name, service->state);
        return -1;
    }

    /* Check dependencies */
    if (!check_dependencies(service))
    {
        k_mutex_unlock(&service_mgr.mutex);
        return -1;
    }

    /* Start service */
    service->state = SERVICE_STATE_STARTING;

    if (service->start)
    {
        int ret = service->start();
        if (ret != 0)
        {
            LOG_ERR("Service '%s' start failed: %d", name, ret);
            service->state = SERVICE_STATE_ERROR;
            k_mutex_unlock(&service_mgr.mutex);
            return ret;
        }
    }

    service->state = SERVICE_STATE_RUNNING;
    service->start_time = k_uptime_get_32();

    k_mutex_unlock(&service_mgr.mutex);

    LOG_INF("Started service '%s'", name);
    return 0;
}

int akira_service_stop(const char *name)
{
    if (!service_mgr.initialized || !name)
    {
        return -1;
    }

    k_mutex_lock(&service_mgr.mutex, K_FOREVER);

    akira_service_t *service = find_service(name);
    if (!service)
    {
        k_mutex_unlock(&service_mgr.mutex);
        return -1;
    }

    if (service->state != SERVICE_STATE_RUNNING)
    {
        k_mutex_unlock(&service_mgr.mutex);
        return 0; /* Already stopped */
    }

    /* Check if other services depend on this */
    for (int i = 0; i < service_mgr.count; i++)
    {
        akira_service_t *other = service_mgr.services[i];
        if (other && other->state == SERVICE_STATE_RUNNING && other->depends_on)
        {
            for (const char **dep = other->depends_on; *dep != NULL; dep++)
            {
                if (strcmp(*dep, name) == 0)
                {
                    LOG_WRN("Cannot stop '%s': '%s' depends on it",
                            name, other->name);
                    k_mutex_unlock(&service_mgr.mutex);
                    return -1;
                }
            }
        }
    }

    service->state = SERVICE_STATE_STOPPING;

    if (service->stop)
    {
        service->stop();
    }

    service->state = SERVICE_STATE_STOPPED;

    k_mutex_unlock(&service_mgr.mutex);

    LOG_INF("Stopped service '%s'", name);
    return 0;
}

int akira_service_restart(const char *name)
{
    akira_service_t *service = find_service(name);
    if (!service)
    {
        return -1;
    }

    int ret = akira_service_stop(name);
    if (ret != 0 && service->state != SERVICE_STATE_STOPPED)
    {
        return ret;
    }

    service->restart_count++;

    return akira_service_start(name);
}

akira_service_state_t akira_service_get_state(const char *name)
{
    akira_service_t *service = find_service(name);
    if (!service)
    {
        return SERVICE_STATE_UNREGISTERED;
    }
    return service->state;
}

bool akira_service_is_running(const char *name)
{
    return akira_service_get_state(name) == SERVICE_STATE_RUNNING;
}

int akira_service_start_all(void)
{
    if (!service_mgr.initialized)
    {
        return -1;
    }

    LOG_INF("Starting all services...");

    /* Sort by priority */
    // TODO: Implement proper topological sort for dependencies

    /* Start in priority order */
    for (int priority = SERVICE_PRIORITY_CRITICAL;
         priority <= SERVICE_PRIORITY_IDLE; priority++)
    {
        for (int i = 0; i < service_mgr.count; i++)
        {
            akira_service_t *service = service_mgr.services[i];
            if (service && service->priority == priority &&
                service->state == SERVICE_STATE_READY)
            {
                akira_service_start(service->name);
            }
        }
    }

    return 0;
}

int akira_service_stop_all(void)
{
    if (!service_mgr.initialized)
    {
        return -1;
    }

    LOG_INF("Stopping all services...");

    /* Stop in reverse priority order */
    for (int priority = SERVICE_PRIORITY_IDLE;
         priority >= SERVICE_PRIORITY_CRITICAL; priority--)
    {
        for (int i = service_mgr.count - 1; i >= 0; i--)
        {
            akira_service_t *service = service_mgr.services[i];
            if (service && service->priority == priority &&
                service->state == SERVICE_STATE_RUNNING)
            {
                akira_service_stop(service->name);
            }
        }
    }

    return 0;
}

akira_service_t *akira_service_get(const char *name)
{
    return find_service(name);
}

akira_service_t *akira_service_get_by_handle(akira_service_handle_t handle)
{
    if (!AKIRA_HANDLE_VALID(handle) || handle >= service_mgr.count)
    {
        return NULL;
    }
    return service_mgr.services[handle];
}

int akira_service_list(akira_service_t **services, int max_count)
{
    if (!services || max_count <= 0)
    {
        return 0;
    }

    int count = MIN(service_mgr.count, max_count);
    for (int i = 0; i < count; i++)
    {
        services[i] = service_mgr.services[i];
    }
    return count;
}

void akira_service_print_status(void)
{
    LOG_INF("=== Service Status ===");
    LOG_INF("Registered services: %d", service_mgr.count);

    static const char *state_names[] = {
        "UNREGISTERED", "REGISTERED", "INITIALIZING", "READY",
        "STARTING", "RUNNING", "STOPPING", "STOPPED", "ERROR"};

    for (int i = 0; i < service_mgr.count; i++)
    {
        akira_service_t *svc = service_mgr.services[i];
        if (svc)
        {
            LOG_INF("  [%d] %s: %s (priority=%d, restarts=%u)",
                    i, svc->name,
                    state_names[svc->state],
                    svc->priority,
                    svc->restart_count);
        }
    }
}

void akira_service_print_all(void)
{
    akira_service_print_status();
}

int akira_service_count(void)
{
    return service_mgr.count;
}

akira_service_t *akira_service_find_by_name(const char *name)
{
    return find_service(name);
}
