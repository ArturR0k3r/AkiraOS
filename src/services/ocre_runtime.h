
#ifndef AKIRA_OCRE_RUNTIME_H
#define AKIRA_OCRE_RUNTIME_H

#include <ocre/api/ocre_api.h>
#include <ocre/ocre_container_runtime/ocre_container_runtime.h>
#include <stddef.h>
#include <stdint.h>

// Use OCRE's actual container/app types
typedef ocre_container_t akira_ocre_container_t;

/**
 * @brief Initialize the OCRE runtime
 * @return 0 on success, negative on error
 */
int ocre_runtime_init(void);

/**
 * @brief Load/create a container application
 * @param name Container name
 * @param binary Binary data (WASM bytecode)
 * @param size Binary size in bytes
 * @return Container ID on success, negative on error
 */
int ocre_runtime_load_app(const char *name, const void *binary, size_t size);

/**
 * @brief Start/run a container application
 * @param name Container name
 * @return 0 on success, negative on error
 */
int ocre_runtime_start_app(const char *name);

/**
 * @brief Stop a running container application
 * @param name Container name
 * @return 0 on success, negative on error
 */
int ocre_runtime_stop_app(const char *name);

/**
 * @brief List all containers
 * @param out_list Output array to store containers
 * @param max_count Maximum number of containers to list
 * @return Number of containers listed, negative on error
 */
int ocre_runtime_list_apps(akira_ocre_container_t *out_list, int max_count);

/**
 * @brief Get container status
 * @param name Container name
 * @return Container status (ocre_container_status_t)
 */
int ocre_runtime_get_status(const char *name);

/**
 * @brief Destroy a container
 * @param name Container name
 * @return 0 on success, negative on error
 */
int ocre_runtime_destroy_app(const char *name);

#endif // AKIRA_OCRE_RUNTIME_H
