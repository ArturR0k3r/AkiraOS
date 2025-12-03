/**
 * @file app_loader.c
 * @brief WASM Application Loader Implementation
 * 
 * Handles loading, validation, and lifecycle of WASM apps.
 * Integrates with OCRE runtime and security system.
 */

#include "app_loader.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>

LOG_MODULE_REGISTER(app_loader, CONFIG_AKIRA_LOG_LEVEL);

/* App control block */
struct app_cb {
	bool in_use;
	struct app_metadata metadata;
	app_state_t state;
	app_source_t source;
	
	/* WASM module data */
	uint8_t *wasm_data;
	size_t wasm_size;
	void *wasm_module;      // WAMR module handle
	void *wasm_instance;    // WAMR instance handle
	
	/* Runtime info */
	uint32_t load_time;
	uint32_t start_time;
	uint32_t total_runtime;
	
	/* Granted capabilities */
	uint64_t capabilities;
};

/* Loader state */
static struct {
	bool initialized;
	struct app_cb apps[APP_MAX_LOADED];
	struct k_mutex mutex;
	uint32_t app_count;
} loader_state;

/**
 * @brief Get app by handle
 */
static struct app_cb *get_app(app_handle_t handle)
{
	if (handle < 0 || handle >= APP_MAX_LOADED) {
		return NULL;
	}
	if (!loader_state.apps[handle].in_use) {
		return NULL;
	}
	return &loader_state.apps[handle];
}

/**
 * @brief Find free app slot
 */
static app_handle_t find_free_slot(void)
{
	for (int i = 0; i < APP_MAX_LOADED; i++) {
		if (!loader_state.apps[i].in_use) {
			return i;
		}
	}
	return -1;
}

/**
 * @brief Parse WASM app metadata
 */
static int parse_app_metadata(const uint8_t *data, size_t size,
                              struct app_metadata *metadata)
{
	// TODO: Implement WASM custom section parsing
	// 1. Validate WASM magic number
	// 2. Find custom section named "akira_app"
	// 3. Parse metadata fields
	// 4. Extract signature if present
	
	if (size < 8) {
		return -EINVAL;
	}
	
	/* Check WASM magic number */
	if (data[0] != 0x00 || data[1] != 0x61 ||
	    data[2] != 0x73 || data[3] != 0x6D) {
		LOG_ERR("Invalid WASM magic number");
		return -EINVAL;
	}
	
	/* Set defaults */
	strncpy(metadata->name, "unknown", APP_NAME_MAX);
	strncpy(metadata->version, "1.0.0", 16);
	strncpy(metadata->author, "unknown", 32);
	metadata->wasm_size = size;
	metadata->required_memory = 64 * 1024;  // 64KB default
	metadata->capabilities = 0;
	metadata->trust_level = 3;  // User app by default
	metadata->is_signed = false;
	metadata->is_verified = false;
	
	LOG_WRN("parse_app_metadata: Using defaults (custom section not implemented)");
	
	return 0;
}

/**
 * @brief Verify app signature
 */
static int verify_signature(struct app_cb *app)
{
	// TODO: Implement signature verification
	// 1. Get public key for trust level
	// 2. Verify Ed25519 signature
	// 3. Update is_verified flag
	
	if (!app->metadata.is_signed) {
		LOG_WRN("App '%s' is not signed", app->metadata.name);
		return -1;
	}
	
	LOG_WRN("verify_signature not implemented");
	return -ENOTSUP;
}

/**
 * @brief Instantiate WASM module
 */
static int instantiate_wasm(struct app_cb *app)
{
	// TODO: Implement WASM instantiation using WAMR
	// 1. Load module from binary
	// 2. Register native functions (API exports)
	// 3. Instantiate module with memory limits
	// 4. Get exported functions
	
	LOG_WRN("instantiate_wasm not implemented");
	return 0;
}

int app_loader_init(void)
{
	if (loader_state.initialized) {
		return 0;
	}
	
	LOG_INF("Initializing app loader");
	
	k_mutex_init(&loader_state.mutex);
	
	for (int i = 0; i < APP_MAX_LOADED; i++) {
		loader_state.apps[i].in_use = false;
	}
	
	loader_state.app_count = 0;
	loader_state.initialized = true;
	
	LOG_INF("App loader initialized (max apps: %d)", APP_MAX_LOADED);
	return 0;
}

app_handle_t app_load_from_flash(const char *path)
{
	if (!loader_state.initialized || !path) {
		return -EINVAL;
	}
	
	LOG_INF("Loading app from flash: %s", path);
	
	// TODO: Implement flash loading
	// 1. Open file
	// 2. Read WASM binary
	// 3. Call app_load_from_memory
	
	struct fs_file_t file;
	fs_file_t_init(&file);
	
	int ret = fs_open(&file, path, FS_O_READ);
	if (ret < 0) {
		LOG_ERR("Failed to open %s: %d", path, ret);
		return ret;
	}
	
	/* Get file size */
	struct fs_dirent dirent;
	ret = fs_stat(path, &dirent);
	if (ret < 0) {
		fs_close(&file);
		return ret;
	}
	
	/* Allocate buffer */
	uint8_t *data = k_malloc(dirent.size);
	if (!data) {
		fs_close(&file);
		return -ENOMEM;
	}
	
	/* Read file */
	ssize_t bytes = fs_read(&file, data, dirent.size);
	fs_close(&file);
	
	if (bytes != dirent.size) {
		k_free(data);
		return -EIO;
	}
	
	/* Extract filename for app name */
	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;
	
	app_handle_t handle = app_load_from_memory(data, dirent.size, name);
	
	/* Note: data ownership transfers to app_load_from_memory */
	if (handle < 0) {
		k_free(data);
	}
	
	return handle;
}

app_handle_t app_load_from_memory(const uint8_t *data, size_t size,
                                   const char *name)
{
	if (!loader_state.initialized || !data || size == 0) {
		return -EINVAL;
	}
	
	k_mutex_lock(&loader_state.mutex, K_FOREVER);
	
	app_handle_t handle = find_free_slot();
	if (handle < 0) {
		k_mutex_unlock(&loader_state.mutex);
		LOG_ERR("No free app slots");
		return -ENOMEM;
	}
	
	struct app_cb *app = &loader_state.apps[handle];
	app->in_use = true;
	app->state = APP_STATE_LOADING;
	
	/* Parse metadata */
	int ret = parse_app_metadata(data, size, &app->metadata);
	if (ret < 0) {
		app->in_use = false;
		k_mutex_unlock(&loader_state.mutex);
		return ret;
	}
	
	/* Override name if provided */
	if (name) {
		strncpy(app->metadata.name, name, APP_NAME_MAX - 1);
		app->metadata.name[APP_NAME_MAX - 1] = '\0';
	}
	
	/* Store WASM data */
	app->wasm_data = k_malloc(size);
	if (!app->wasm_data) {
		app->in_use = false;
		k_mutex_unlock(&loader_state.mutex);
		return -ENOMEM;
	}
	memcpy(app->wasm_data, data, size);
	app->wasm_size = size;
	
	/* Instantiate WASM module */
	ret = instantiate_wasm(app);
	if (ret < 0) {
		k_free(app->wasm_data);
		app->in_use = false;
		k_mutex_unlock(&loader_state.mutex);
		return ret;
	}
	
	app->state = APP_STATE_LOADED;
	app->source = APP_SOURCE_FLASH;
	app->load_time = k_uptime_get_32();
	app->capabilities = 0;
	
	loader_state.app_count++;
	
	k_mutex_unlock(&loader_state.mutex);
	
	LOG_INF("Loaded app '%s' (handle=%d, size=%zu)", 
	        app->metadata.name, handle, size);
	
	return handle;
}

app_handle_t app_load_embedded(const char *name)
{
	if (!loader_state.initialized || !name) {
		return -EINVAL;
	}
	
	// TODO: Look up embedded app by name
	// - Search linker-defined symbol table
	// - Get embedded WASM binary
	// - Call app_load_from_memory
	
	LOG_WRN("app_load_embedded not implemented");
	return -ENOTSUP;
}

int app_unload(app_handle_t handle)
{
	k_mutex_lock(&loader_state.mutex, K_FOREVER);
	
	struct app_cb *app = get_app(handle);
	if (!app) {
		k_mutex_unlock(&loader_state.mutex);
		return -ENOENT;
	}
	
	/* Stop if running */
	if (app->state == APP_STATE_RUNNING) {
		app_stop(handle);
	}
	
	// TODO: Destroy WASM instance and module
	
	/* Free resources */
	if (app->wasm_data) {
		k_free(app->wasm_data);
		app->wasm_data = NULL;
	}
	
	LOG_INF("Unloaded app '%s'", app->metadata.name);
	
	app->in_use = false;
	loader_state.app_count--;
	
	k_mutex_unlock(&loader_state.mutex);
	return 0;
}

int app_start(app_handle_t handle)
{
	struct app_cb *app = get_app(handle);
	if (!app) {
		return -ENOENT;
	}
	
	if (app->state != APP_STATE_LOADED && app->state != APP_STATE_PAUSED) {
		return -EINVAL;
	}
	
	// TODO: Start WASM execution
	// 1. Create execution thread/task
	// 2. Call WASM main/init function
	// 3. Set up event loop
	
	app->state = APP_STATE_RUNNING;
	app->start_time = k_uptime_get_32();
	
	LOG_INF("Started app '%s'", app->metadata.name);
	return 0;
}

int app_stop(app_handle_t handle)
{
	struct app_cb *app = get_app(handle);
	if (!app) {
		return -ENOENT;
	}
	
	if (app->state != APP_STATE_RUNNING && app->state != APP_STATE_PAUSED) {
		return -EINVAL;
	}
	
	// TODO: Stop WASM execution
	// 1. Signal execution thread to stop
	// 2. Wait for graceful shutdown
	// 3. Force terminate if needed
	
	app->total_runtime += k_uptime_get_32() - app->start_time;
	app->state = APP_STATE_LOADED;
	
	LOG_INF("Stopped app '%s' (runtime: %u ms)", 
	        app->metadata.name, app->total_runtime);
	return 0;
}

int app_pause(app_handle_t handle)
{
	struct app_cb *app = get_app(handle);
	if (!app) {
		return -ENOENT;
	}
	
	if (app->state != APP_STATE_RUNNING) {
		return -EINVAL;
	}
	
	// TODO: Pause WASM execution
	
	app->total_runtime += k_uptime_get_32() - app->start_time;
	app->state = APP_STATE_PAUSED;
	
	LOG_DBG("Paused app '%s'", app->metadata.name);
	return 0;
}

int app_resume(app_handle_t handle)
{
	struct app_cb *app = get_app(handle);
	if (!app) {
		return -ENOENT;
	}
	
	if (app->state != APP_STATE_PAUSED) {
		return -EINVAL;
	}
	
	// TODO: Resume WASM execution
	
	app->state = APP_STATE_RUNNING;
	app->start_time = k_uptime_get_32();
	
	LOG_DBG("Resumed app '%s'", app->metadata.name);
	return 0;
}

int app_get_info(app_handle_t handle, struct app_info *info)
{
	if (!info) {
		return -EINVAL;
	}
	
	struct app_cb *app = get_app(handle);
	if (!app) {
		return -ENOENT;
	}
	
	info->handle = handle;
	info->metadata = app->metadata;
	info->state = app->state;
	info->source = app->source;
	info->load_time = app->load_time;
	info->runtime_ms = app->total_runtime;
	
	if (app->state == APP_STATE_RUNNING) {
		info->runtime_ms += k_uptime_get_32() - app->start_time;
	}
	
	return 0;
}

int app_list(app_handle_t *handles, int max_count)
{
	if (!handles || max_count <= 0) {
		return 0;
	}
	
	int count = 0;
	for (int i = 0; i < APP_MAX_LOADED && count < max_count; i++) {
		if (loader_state.apps[i].in_use) {
			handles[count++] = i;
		}
	}
	
	return count;
}

int akira_app_verify_signature(app_handle_t handle)
{
	struct app_cb *app = get_app(handle);
	if (!app) {
		return -ENOENT;
	}
	
	return verify_signature(app);
}

int app_grant_capability(app_handle_t handle, uint32_t capability)
{
	struct app_cb *app = get_app(handle);
	if (!app) {
		return -ENOENT;
	}
	
	app->capabilities |= capability;
	
	LOG_DBG("Granted capability 0x%08X to '%s'", capability, app->metadata.name);
	return 0;
}

int app_revoke_capability(app_handle_t handle, uint32_t capability)
{
	struct app_cb *app = get_app(handle);
	if (!app) {
		return -ENOENT;
	}
	
	app->capabilities &= ~capability;
	
	LOG_DBG("Revoked capability 0x%08X from '%s'", capability, app->metadata.name);
	return 0;
}

bool app_has_capability(app_handle_t handle, uint32_t capability)
{
	struct app_cb *app = get_app(handle);
	if (!app) {
		return false;
	}
	
	return (app->capabilities & capability) == capability;
}

void *app_get_wasm_module(app_handle_t handle)
{
	struct app_cb *app = get_app(handle);
	if (!app) {
		return NULL;
	}
	
	return app->wasm_module;
}

int app_send_event(app_handle_t handle, uint32_t event_type,
                   const void *data, size_t size)
{
	struct app_cb *app = get_app(handle);
	if (!app) {
		return -ENOENT;
	}
	
	if (app->state != APP_STATE_RUNNING) {
		return -EINVAL;
	}
	
	// TODO: Implement event delivery
	// 1. Queue event for app
	// 2. Call app's event handler function
	
	(void)event_type;
	(void)data;
	(void)size;
	
	LOG_WRN("app_send_event not implemented");
	return 0;
}
