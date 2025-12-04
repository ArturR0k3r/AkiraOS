/**
 * @file akira_runtime.h
 * @brief AkiraOS Runtime - OCRE Integration Layer
 *
 * Provides a simplified interface to the OCRE container runtime.
 * Uses container_id directly (returned by OCRE) instead of name lookups.
 *
 * Key design principles:
 * - Use OCRE's API directly with minimal wrapping
 * - Store container_id after creation, use it for all operations
 * - WASM binaries are saved to /lfs/ocre/images/{name}.bin (OCRE's expected path)
 * - No name-based lookups - avoids async timing issues
 */

#ifndef AKIRA_RUNTIME_H
#define AKIRA_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Container status (mirrors OCRE's status)
 */
typedef enum {
    AKIRA_CONTAINER_UNKNOWN = 0,
    AKIRA_CONTAINER_CREATED = 1,
    AKIRA_CONTAINER_RUNNING = 2,
    AKIRA_CONTAINER_STOPPED = 3,
    AKIRA_CONTAINER_DESTROYED = 4,
    AKIRA_CONTAINER_ERROR = 5,
} akira_container_status_t;

/**
 * @brief Container info for listing
 */
typedef struct {
    int id;
    char name[32];
    akira_container_status_t status;
} akira_container_info_t;

/**
 * @brief Initialize the Akira runtime (OCRE + storage)
 *
 * Must be called once at startup before any other akira_runtime functions.
 *
 * @return 0 on success, negative error code on failure
 */
int akira_runtime_init(void);

/**
 * @brief Check if runtime is initialized
 *
 * @return true if initialized
 */
bool akira_runtime_is_initialized(void);

/**
 * @brief Install a WASM app
 *
 * Saves the binary to OCRE's expected path (/lfs/ocre/images/{name}.bin)
 * and creates an OCRE container.
 *
 * @param name App name (used as filename and container name)
 * @param binary WASM binary data
 * @param size Binary size in bytes
 * @return Container ID (>= 0) on success, negative error code on failure
 */
int akira_runtime_install(const char *name, const void *binary, size_t size);

/**
 * @brief Start a container by ID
 *
 * @param container_id Container ID returned from akira_runtime_install
 * @return 0 on success, negative error code on failure
 */
int akira_runtime_start(int container_id);

/**
 * @brief Stop a container by ID
 *
 * @param container_id Container ID
 * @return 0 on success, negative error code on failure
 */
int akira_runtime_stop(int container_id);

/**
 * @brief Destroy a container by ID
 *
 * Destroys the OCRE container. Does NOT delete the binary file.
 * The container can be re-created by calling akira_runtime_install again.
 *
 * @param container_id Container ID
 * @return 0 on success, negative error code on failure
 */
int akira_runtime_destroy(int container_id);

/**
 * @brief Uninstall an app completely
 *
 * Destroys the container (if exists) and deletes the binary file.
 *
 * @param name App name
 * @param container_id Container ID (or -1 if not known)
 * @return 0 on success, negative error code on failure
 */
int akira_runtime_uninstall(const char *name, int container_id);

/**
 * @brief Get container status by ID
 *
 * @param container_id Container ID
 * @return Container status, or AKIRA_CONTAINER_UNKNOWN if invalid ID
 */
akira_container_status_t akira_runtime_get_status(int container_id);

/**
 * @brief List all containers
 *
 * @param out_list Output array for container info
 * @param max_count Maximum entries to return
 * @return Number of containers found, negative on error
 */
int akira_runtime_list(akira_container_info_t *out_list, int max_count);

/**
 * @brief Save WASM binary to OCRE's image path
 *
 * Helper function to save binary to /lfs/ocre/images/{name}.bin
 * This is where OCRE expects to find container binaries.
 *
 * @param name App name (without .bin extension)
 * @param binary WASM binary data
 * @param size Binary size
 * @return 0 on success, negative error code on failure
 */
int akira_runtime_save_binary(const char *name, const void *binary, size_t size);

/**
 * @brief Delete WASM binary from OCRE's image path
 *
 * @param name App name
 * @return 0 on success, negative error code on failure
 */
int akira_runtime_delete_binary(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_RUNTIME_H */
