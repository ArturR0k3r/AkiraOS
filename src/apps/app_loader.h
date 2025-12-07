/**
 * @file app_loader.h
 * @brief WASM Application Loader
 *
 * Loads, validates, and instantiates WASM applications.
 * Handles app signing verification and capability granting.
 */

#ifndef AKIRA_APP_LOADER_H
#define AKIRA_APP_LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Maximum app name length
 */
#define APP_NAME_MAX 32

/**
 * @brief Maximum loaded apps
 */
#define APP_MAX_LOADED 8

	/**
	 * @brief App load source
	 */
	typedef enum
	{
		APP_SOURCE_FLASH,	// Internal flash storage
		APP_SOURCE_SD,		// SD card
		APP_SOURCE_NETWORK, // Downloaded from network
		APP_SOURCE_EMBEDDED // Embedded in firmware
	} app_source_t;

	/**
	 * @brief App state
	 */
	typedef enum
	{
		APP_STATE_UNLOADED = 0,
		APP_STATE_LOADING,
		APP_STATE_LOADED,
		APP_STATE_RUNNING,
		APP_STATE_PAUSED,
		APP_STATE_ERROR
	} app_state_t;

	/**
	 * @brief App handle
	 */
	typedef int app_handle_t;

	/**
	 * @brief App metadata
	 */
	struct app_metadata
	{
		char name[APP_NAME_MAX];
		char version[16];
		char author[32];
		uint32_t wasm_size;
		uint32_t required_memory;
		uint32_t capabilities; // Requested capability flags
		uint8_t trust_level;   // Required trust level
		uint8_t signature[64]; // Ed25519 signature
		bool is_signed;
		bool is_verified;
	};

	/**
	 * @brief App info
	 */
	struct app_info
	{
		app_handle_t handle;
		struct app_metadata metadata;
		app_state_t state;
		app_source_t source;
		uint32_t load_time;
		uint32_t runtime_ms;
	};

	/**
	 * @brief Initialize app loader
	 * @return 0 on success
	 */
	int app_loader_init(void);

	/**
	 * @brief Load app from flash
	 * @param path Path to WASM file
	 * @return App handle or negative error
	 */
	app_handle_t app_load_from_flash(const char *path);

	/**
	 * @brief Load app from memory
	 * @param data WASM binary data
	 * @param size Data size
	 * @param name App name
	 * @return App handle or negative error
	 */
	app_handle_t app_load_from_memory(const uint8_t *data, size_t size,
									  const char *name);

	/**
	 * @brief Load embedded app
	 * @param name Embedded app name
	 * @return App handle or negative error
	 */
	app_handle_t app_load_embedded(const char *name);

	/**
	 * @brief Unload app
	 * @param handle App handle
	 * @return 0 on success
	 */
	int app_unload(app_handle_t handle);

	/**
	 * @brief Start app execution
	 * @param handle App handle
	 * @return 0 on success
	 */
	int app_start(app_handle_t handle);

	/**
	 * @brief Stop app execution
	 * @param handle App handle
	 * @return 0 on success
	 */
	int app_stop(app_handle_t handle);

	/**
	 * @brief Pause app execution
	 * @param handle App handle
	 * @return 0 on success
	 */
	int app_pause(app_handle_t handle);

	/**
	 * @brief Resume app execution
	 * @param handle App handle
	 * @return 0 on success
	 */
	int app_resume(app_handle_t handle);

	/**
	 * @brief Get app info
	 * @param handle App handle
	 * @param info Output for app info
	 * @return 0 on success
	 */
	int app_get_info(app_handle_t handle, struct app_info *info);

	/**
	 * @brief List loaded apps
	 * @param handles Output array for handles
	 * @param max_count Maximum handles to return
	 * @return Number of apps found
	 */
	int app_list(app_handle_t *handles, int max_count);

	/**
	 * @brief Verify app signature
	 * @param handle App handle
	 * @return 0 if valid, -1 if invalid
	 */
	int akira_app_verify_signature(app_handle_t handle);

	/**
	 * @brief Grant capability to app
	 * @param handle App handle
	 * @param capability Capability flag
	 * @return 0 on success
	 */
	int app_grant_capability(app_handle_t handle, uint32_t capability);

	/**
	 * @brief Revoke capability from app
	 * @param handle App handle
	 * @param capability Capability flag
	 * @return 0 on success
	 */
	int app_revoke_capability(app_handle_t handle, uint32_t capability);

	/**
	 * @brief Check if app has capability
	 * @param handle App handle
	 * @param capability Capability flag
	 * @return true if granted
	 */
	bool app_has_capability(app_handle_t handle, uint32_t capability);

	/**
	 * @brief Get app WASM module (for runtime)
	 * @param handle App handle
	 * @return WASM module pointer or NULL
	 */
	void *app_get_wasm_module(app_handle_t handle);

	/**
	 * @brief Send event to app
	 * @param handle App handle
	 * @param event_type Event type
	 * @param data Event data
	 * @param size Data size
	 * @return 0 on success
	 */
	int app_send_event(app_handle_t handle, uint32_t event_type,
					   const void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_APP_LOADER_H */
