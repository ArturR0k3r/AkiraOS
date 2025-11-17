#include "ocre_runtime.h"
#include <ocre/api/ocre_api.h>
#include <ocre/ocre_container_runtime/ocre_container_runtime.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(wasm_app_mgr, LOG_LEVEL_INF);

typedef ocre_container_t akira_ocre_container_t;

int wasm_app_upload(const char *name, const void *binary, size_t size, uint32_t version)
{
	if (!name || !binary || size == 0) {
		LOG_ERR("Invalid parameters for WASM app upload");
		return -1;
	}

	LOG_INF("Uploading WASM app: %s (size: %zu, version: %u)", name, size, version);

	int container_id = ocre_runtime_load_app(name, binary, size);

	if (container_id >= 0) {
		LOG_INF("WASM app uploaded: %s (container ID: %d)", name, container_id);
		return container_id;
	}

	LOG_ERR("Failed to upload WASM app: %s", name);
	return -1;
}

int wasm_app_update(const char *name, const void *binary, size_t size, uint32_t version)
{
	if (!name || !binary || size == 0) {
		LOG_ERR("Invalid parameters for WASM app update");
		return -1;
	}

	LOG_INF("Updating WASM app: %s (size: %zu, version: %u)", name, size, version);

	int status = ocre_runtime_get_status(name);

	if (status != CONTAINER_STATUS_UNKNOWN)
	{
		
		LOG_INF("Stopping existing container: %s", name);
		ocre_runtime_stop_app(name);

		LOG_INF("Destroying existing container: %s", name);
		ocre_runtime_destroy_app(name);
	}

	
	int container_id = ocre_runtime_load_app(name, binary, size);

	if (container_id >= 0)
	{
		LOG_INF("WASM app updated successfully: %s (container ID: %d)", name, container_id);
		return container_id;
	}

	LOG_ERR("Failed to update WASM app: %s", name);
	return -1;
}

int wasm_app_list(akira_ocre_container_t *out_list, int max_count)
{
	if (!out_list || max_count <= 0)
	{
		LOG_ERR("Invalid parameters for WASM app list");
		return -1;
	}

	
	int count = ocre_runtime_list_apps(out_list, max_count);

	if (count >= 0)
	{
		LOG_INF("Listed %d WASM apps", count);
	}
	else
	{
		LOG_ERR("Failed to list WASM apps");
	}

	return count;
}

int wasm_app_start(const char *name)
{
	if (!name)
	{
		LOG_ERR("Invalid name for WASM app start");
		return -1;
	}

	LOG_INF("Starting WASM app: %s", name);
	return ocre_runtime_start_app(name);
}

int wasm_app_stop(const char *name)
{
	if (!name)
	{
		LOG_ERR("Invalid name for WASM app stop");
		return -1;
	}

	LOG_INF("Stopping WASM app: %s", name);
	return ocre_runtime_stop_app(name);
}

int wasm_app_delete(const char *name)
{
	if (!name)
	{
		LOG_ERR("Invalid name for WASM app delete");
		return -1;
	}

	LOG_INF("Deleting WASM app: %s", name);

	
	ocre_runtime_stop_app(name);

	
	return ocre_runtime_destroy_app(name);
}
