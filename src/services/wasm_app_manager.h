/**
 * @file wasm_app_manager.h
 * @brief AkiraOS WASM App Upload/Update Manager Interface
 */

#ifndef AKIRA_WASM_APP_MANAGER_H
#define AKIRA_WASM_APP_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <ocre/ocre_container_runtime/ocre_container_runtime.h>

// Type definition for container type (uses OCRE's actual type)
typedef ocre_container_t akira_ocre_container_t;

/**
 * @brief Upload a new WASM application
 * @param name Application name
 * @param binary WASM binary data
 * @param size Binary size in bytes
 * @param version Application version
 * @return Container ID on success, negative on error
 */
int wasm_app_upload(const char *name, const void *binary, size_t size, uint32_t version);

/**
 * @brief Update an existing WASM application
 * @param name Application name
 * @param binary WASM binary data
 * @param size Binary size in bytes
 * @param version Application version
 * @return Container ID on success, negative on error
 */
int wasm_app_update(const char *name, const void *binary, size_t size, uint32_t version);

/**
 * @brief List all WASM applications
 * @param out_list Output array to store applications
 * @param max_count Maximum number of applications to list
 * @return Number of applications listed, negative on error
 */
int wasm_app_list(akira_ocre_container_t *out_list, int max_count);

/**
 * @brief Start a WASM application
 * @param name Application name
 * @return 0 on success, negative on error
 */
int wasm_app_start(const char *name);

/**
 * @brief Stop a running WASM application
 * @param name Application name
 * @return 0 on success, negative on error
 */
int wasm_app_stop(const char *name);

/**
 * @brief Delete a WASM application
 * @param name Application name
 * @return 0 on success, negative on error
 */
int wasm_app_delete(const char *name);

#endif // AKIRA_WASM_APP_MANAGER_H
