
/**
 * @file ocre_runtime.c
 * @brief AkiraOS OCRE (On-Chip Runtime Environment) Integration
 */

#include "ocre_runtime.h"
#include <ocre/api/ocre_api.h>
#include <ocre/ocre_container_runtime/ocre_container_runtime.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_ocre, LOG_LEVEL_INF);

// Global OCRE runtime context
static ocre_cs_ctx g_ocre_ctx;
static bool g_ocre_initialized = false;

// Initialize OCRE runtime (should be called once at startup)
int ocre_runtime_init(void)
{
    if (g_ocre_initialized)
    {
        LOG_WRN("OCRE runtime already initialized");
        return 0;
    }

    ocre_container_init_arguments_t args = {0};
    ocre_container_runtime_status_t status = ocre_container_runtime_init(&g_ocre_ctx, &args);

    if (status == RUNTIME_STATUS_INITIALIZED)
    {
        g_ocre_initialized = true;
        LOG_INF("OCRE runtime initialized successfully");
        return 0;
    }

    LOG_ERR("Failed to initialize OCRE runtime: %d", status);
    return -1;
}

// Find container ID by name (helper function)
static int find_container_by_name(const char *name)
{
    if (!name || !g_ocre_initialized)
    {
        return -1;
    }

    for (int i = 0; i < CONFIG_MAX_CONTAINERS; i++)
    {
        if (g_ocre_ctx.containers[i].container_runtime_status != CONTAINER_STATUS_UNKNOWN &&
            g_ocre_ctx.containers[i].container_runtime_status != CONTAINER_STATUS_DESTROYED)
        {
            if (strcmp(g_ocre_ctx.containers[i].ocre_container_data.name, name) == 0)
            {
                return i;
            }
        }
    }

    return -1;
}

int ocre_runtime_load_app(const char *name, const void *binary, size_t size)
{
    if (!g_ocre_initialized)
    {
        LOG_ERR("OCRE runtime not initialized");
        return -1;
    }

    // Create container data structure
    ocre_container_data_t container_data = {0};
    int container_id = -1;

    // Copy name to container data
    strncpy(container_data.name, name, OCRE_MODULE_NAME_LEN - 1);
    strncpy(container_data.sha256, name, OCRE_SHA256_LEN - 1); // Use name as SHA for now

    // Set default sizes
    container_data.heap_size = 0;  // Use defaults
    container_data.stack_size = 0; // Use defaults
    container_data.timers = 0;
    container_data.watchdog_interval = 0;

    // Create the container
    ocre_container_status_t status = ocre_container_runtime_create_container(
        &g_ocre_ctx, &container_data, &container_id, NULL);

    if (status == CONTAINER_STATUS_CREATED)
    {
        LOG_INF("Container created successfully: %s (ID: %d)", name, container_id);
        return container_id;
    }

    LOG_ERR("Failed to create container: %s, status: %d", name, status);
    return -1;
}

int ocre_runtime_start_app(const char *name)
{
    if (!g_ocre_initialized)
    {
        LOG_ERR("OCRE runtime not initialized");
        return -1;
    }

    int container_id = find_container_by_name(name);
    if (container_id < 0)
    {
        LOG_ERR("Container not found: %s", name);
        return -1;
    }

    // Run the container
    ocre_container_status_t status = ocre_container_runtime_run_container(container_id, NULL);

    if (status == CONTAINER_STATUS_RUNNING)
    {
        LOG_INF("Container started: %s (ID: %d)", name, container_id);
        return 0;
    }

    LOG_ERR("Failed to start container: %s, status: %d", name, status);
    return -1;
}

int ocre_runtime_stop_app(const char *name)
{
    if (!g_ocre_initialized)
    {
        LOG_ERR("OCRE runtime not initialized");
        return -1;
    }

    int container_id = find_container_by_name(name);
    if (container_id < 0)
    {
        LOG_ERR("Container not found: %s", name);
        return -1;
    }

    // Stop the container
    ocre_container_status_t status = ocre_container_runtime_stop_container(container_id, NULL);

    if (status == CONTAINER_STATUS_STOPPED)
    {
        LOG_INF("Container stopped: %s (ID: %d)", name, container_id);
        return 0;
    }

    LOG_ERR("Failed to stop container: %s, status: %d", name, status);
    return -1;
}

int ocre_runtime_list_apps(akira_ocre_container_t *out_list, int max_count)
{
    if (!g_ocre_initialized || !out_list || max_count <= 0)
    {
        return -1;
    }

    int count = 0;
    for (int i = 0; i < CONFIG_MAX_CONTAINERS && count < max_count; i++)
    {
        if (g_ocre_ctx.containers[i].container_runtime_status != CONTAINER_STATUS_UNKNOWN &&
            g_ocre_ctx.containers[i].container_runtime_status != CONTAINER_STATUS_DESTROYED)
        {
            // Copy container information
            memcpy(&out_list[count], &g_ocre_ctx.containers[i], sizeof(akira_ocre_container_t));
            count++;
        }
    }

    LOG_INF("Listed %d containers", count);
    return count;
}

int ocre_runtime_get_status(const char *name)
{
    if (!g_ocre_initialized)
    {
        return CONTAINER_STATUS_UNKNOWN;
    }

    int container_id = find_container_by_name(name);
    if (container_id < 0)
    {
        return CONTAINER_STATUS_UNKNOWN;
    }

    return ocre_container_runtime_get_container_status(&g_ocre_ctx, container_id);
}

int ocre_runtime_destroy_app(const char *name)
{
    if (!g_ocre_initialized)
    {
        LOG_ERR("OCRE runtime not initialized");
        return -1;
    }

    int container_id = find_container_by_name(name);
    if (container_id < 0)
    {
        LOG_ERR("Container not found: %s", name);
        return -1;
    }

    // Destroy the container
    ocre_container_status_t status = ocre_container_runtime_destroy_container(
        &g_ocre_ctx, container_id, NULL);

    if (status == CONTAINER_STATUS_DESTROYED)
    {
        LOG_INF("Container destroyed: %s (ID: %d)", name, container_id);
        return 0;
    }

    LOG_ERR("Failed to destroy container: %s, status: %d", name, status);
    return -1;
}
