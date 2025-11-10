
/**
 * @file ocre_runtime.c
 * @brief AkiraOS OCRE (On-Chip Runtime Environment) Integration
 */

#include "ocre_runtime.h"
#include <ocre/api/ocre_api.h>
#include <ocre/ocre_container_runtime/ocre_container_runtime.h>

int ocre_runtime_load_app(const char *name, const void *binary, size_t size)
{
    // Load a container/app using OCRE API
    return ocre_container_runtime_load(name, binary, size);
}

int ocre_runtime_start_app(const char *name)
{
    // Start a container/app using OCRE API
    return ocre_container_runtime_start(name);
}

int ocre_runtime_stop_app(const char *name)
{
    // Stop a container/app using OCRE API
    return ocre_container_runtime_stop(name);
}

int ocre_runtime_list_apps(akira_ocre_container_t *out_list, int max_count)
{
    // List containers/apps using OCRE API
    return ocre_container_runtime_list(out_list, max_count);
}
