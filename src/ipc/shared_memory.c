/**
 * @file shared_memory.c
 * @brief Shared Memory IPC Implementation
 * 
 * Provides zero-copy data sharing between WASM apps.
 * Memory is allocated from a dedicated pool with permission enforcement.
 */

#include "shared_memory.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(shmem, CONFIG_AKIRA_LOG_LEVEL);

/* Shared memory pool size */
#define SHMEM_POOL_SIZE     (64 * 1024)  // 64KB total

/* Per-app permission entry */
struct shmem_perm_entry {
	uint32_t app_id;
	shmem_perm_t perm;
};

#define SHMEM_MAX_PERM_ENTRIES  8

/* Shared memory region */
struct shmem_region {
	bool in_use;
	char name[SHMEM_MAX_NAME_LEN];
	void *data;
	size_t size;
	uint32_t owner_id;
	uint32_t ref_count;
	shmem_perm_t default_perm;
	struct shmem_perm_entry perm_entries[SHMEM_MAX_PERM_ENTRIES];
	uint32_t perm_count;
	struct k_mutex lock;
	bool is_locked;
	uint32_t lock_owner;
};

/* Subsystem state */
static struct {
	bool initialized;
	struct shmem_region regions[SHMEM_MAX_REGIONS];
	struct k_mutex global_mutex;
	
	/* Memory pool */
	uint8_t pool[SHMEM_POOL_SIZE];
	size_t pool_used;
} shmem_state;

/**
 * @brief Get current app ID
 */
static uint32_t get_current_app_id(void)
{
	// TODO: Get actual caller app ID from WASM context or thread
	return 0;
}

/**
 * @brief Find region by name
 */
static struct shmem_region *find_region_by_name(const char *name)
{
	for (int i = 0; i < SHMEM_MAX_REGIONS; i++) {
		if (shmem_state.regions[i].in_use &&
		    strcmp(shmem_state.regions[i].name, name) == 0) {
			return &shmem_state.regions[i];
		}
	}
	return NULL;
}

/**
 * @brief Find region by handle
 */
static struct shmem_region *get_region(shmem_handle_t handle)
{
	if (handle < 0 || handle >= SHMEM_MAX_REGIONS) {
		return NULL;
	}
	if (!shmem_state.regions[handle].in_use) {
		return NULL;
	}
	return &shmem_state.regions[handle];
}

/**
 * @brief Check permission for app
 */
static bool check_permission(struct shmem_region *region, uint32_t app_id, 
                             shmem_perm_t required)
{
	/* Owner has full access */
	if (app_id == region->owner_id) {
		return true;
	}
	
	/* Check specific permission entries */
	for (uint32_t i = 0; i < region->perm_count; i++) {
		if (region->perm_entries[i].app_id == app_id) {
			return (region->perm_entries[i].perm & required) == required;
		}
	}
	
	/* Fall back to default permission */
	return (region->default_perm & required) == required;
}

/**
 * @brief Allocate from pool
 */
static void *pool_alloc(size_t size)
{
	// TODO: Implement proper memory allocator
	// - First fit or best fit algorithm
	// - Track free blocks
	// - Support deallocation
	
	/* Simple bump allocator for now */
	size_t aligned_size = (size + 7) & ~7;  // 8-byte alignment
	
	if (shmem_state.pool_used + aligned_size > SHMEM_POOL_SIZE) {
		return NULL;
	}
	
	void *ptr = &shmem_state.pool[shmem_state.pool_used];
	shmem_state.pool_used += aligned_size;
	
	return ptr;
}

/**
 * @brief Free to pool
 */
static void pool_free(void *ptr, size_t size)
{
	// TODO: Implement proper deallocation
	// - Mark block as free
	// - Coalesce adjacent free blocks
	
	(void)ptr;
	(void)size;
	
	LOG_WRN("pool_free not implemented (memory leak)");
}

int shmem_init(void)
{
	if (shmem_state.initialized) {
		return 0;
	}
	
	LOG_INF("Initializing shared memory subsystem");
	
	k_mutex_init(&shmem_state.global_mutex);
	
	for (int i = 0; i < SHMEM_MAX_REGIONS; i++) {
		shmem_state.regions[i].in_use = false;
		k_mutex_init(&shmem_state.regions[i].lock);
	}
	
	shmem_state.pool_used = 0;
	shmem_state.initialized = true;
	
	LOG_INF("Shared memory initialized (pool: %d bytes)", SHMEM_POOL_SIZE);
	return 0;
}

shmem_handle_t shmem_create(const char *name, size_t size, shmem_perm_t default_perm)
{
	if (!shmem_state.initialized) {
		return -ENODEV;
	}
	
	if (!name || size == 0) {
		return -EINVAL;
	}
	
	k_mutex_lock(&shmem_state.global_mutex, K_FOREVER);
	
	/* Check if name already exists */
	if (find_region_by_name(name)) {
		k_mutex_unlock(&shmem_state.global_mutex);
		LOG_ERR("Region '%s' already exists", name);
		return -EEXIST;
	}
	
	/* Find free slot */
	int slot = -1;
	for (int i = 0; i < SHMEM_MAX_REGIONS; i++) {
		if (!shmem_state.regions[i].in_use) {
			slot = i;
			break;
		}
	}
	
	if (slot < 0) {
		k_mutex_unlock(&shmem_state.global_mutex);
		LOG_ERR("No free region slots");
		return -ENOMEM;
	}
	
	/* Allocate memory */
	void *data = pool_alloc(size);
	if (!data) {
		k_mutex_unlock(&shmem_state.global_mutex);
		LOG_ERR("Failed to allocate %zu bytes", size);
		return -ENOMEM;
	}
	
	/* Initialize region */
	struct shmem_region *region = &shmem_state.regions[slot];
	region->in_use = true;
	strncpy(region->name, name, SHMEM_MAX_NAME_LEN - 1);
	region->name[SHMEM_MAX_NAME_LEN - 1] = '\0';
	region->data = data;
	region->size = size;
	region->owner_id = get_current_app_id();
	region->ref_count = 1;
	region->default_perm = default_perm;
	region->perm_count = 0;
	region->is_locked = false;
	
	memset(data, 0, size);
	
	k_mutex_unlock(&shmem_state.global_mutex);
	
	LOG_INF("Created shared memory '%s' (size=%zu, handle=%d)", name, size, slot);
	return slot;
}

shmem_handle_t shmem_open(const char *name, shmem_perm_t requested_perm)
{
	if (!shmem_state.initialized || !name) {
		return -EINVAL;
	}
	
	k_mutex_lock(&shmem_state.global_mutex, K_FOREVER);
	
	struct shmem_region *region = find_region_by_name(name);
	if (!region) {
		k_mutex_unlock(&shmem_state.global_mutex);
		return -ENOENT;
	}
	
	/* Check permission */
	if (!check_permission(region, get_current_app_id(), requested_perm)) {
		k_mutex_unlock(&shmem_state.global_mutex);
		LOG_WRN("Permission denied for region '%s'", name);
		return -EACCES;
	}
	
	region->ref_count++;
	
	/* Return handle (index in array) */
	int handle = region - shmem_state.regions;
	
	k_mutex_unlock(&shmem_state.global_mutex);
	
	LOG_DBG("Opened shared memory '%s' (handle=%d)", name, handle);
	return handle;
}

int shmem_close(shmem_handle_t handle)
{
	struct shmem_region *region = get_region(handle);
	if (!region) {
		return -EINVAL;
	}
	
	k_mutex_lock(&shmem_state.global_mutex, K_FOREVER);
	
	if (region->ref_count > 0) {
		region->ref_count--;
	}
	
	/* Auto-destroy when last reference is closed */
	if (region->ref_count == 0) {
		pool_free(region->data, region->size);
		region->in_use = false;
		LOG_INF("Destroyed shared memory '%s'", region->name);
	}
	
	k_mutex_unlock(&shmem_state.global_mutex);
	return 0;
}

int shmem_destroy(shmem_handle_t handle)
{
	struct shmem_region *region = get_region(handle);
	if (!region) {
		return -EINVAL;
	}
	
	/* Only owner can destroy */
	if (region->owner_id != get_current_app_id()) {
		return -EACCES;
	}
	
	k_mutex_lock(&shmem_state.global_mutex, K_FOREVER);
	
	pool_free(region->data, region->size);
	region->in_use = false;
	
	k_mutex_unlock(&shmem_state.global_mutex);
	
	LOG_INF("Force destroyed shared memory '%s'", region->name);
	return 0;
}

void *shmem_map(shmem_handle_t handle)
{
	struct shmem_region *region = get_region(handle);
	if (!region) {
		return NULL;
	}
	
	// TODO: For WASM, need to map into linear memory
	// - Use WAMR's wasm_runtime_module_malloc()
	// - Copy data into WASM space
	// - Return WASM-accessible pointer
	
	LOG_WRN("shmem_map: Direct pointer (not WASM-safe)");
	return region->data;
}

int shmem_unmap(shmem_handle_t handle)
{
	struct shmem_region *region = get_region(handle);
	if (!region) {
		return -EINVAL;
	}
	
	// TODO: For WASM, free the mapped memory
	
	return 0;
}

int shmem_get_info(shmem_handle_t handle, struct shmem_info *info)
{
	struct shmem_region *region = get_region(handle);
	if (!region || !info) {
		return -EINVAL;
	}
	
	strncpy(info->name, region->name, SHMEM_MAX_NAME_LEN);
	info->size = region->size;
	info->owner_id = region->owner_id;
	info->ref_count = region->ref_count;
	info->default_perm = region->default_perm;
	
	return 0;
}

int shmem_set_permission(shmem_handle_t handle, uint32_t app_id, shmem_perm_t perm)
{
	struct shmem_region *region = get_region(handle);
	if (!region) {
		return -EINVAL;
	}
	
	/* Only owner can set permissions */
	if (region->owner_id != get_current_app_id()) {
		return -EACCES;
	}
	
	/* Find existing entry or add new */
	for (uint32_t i = 0; i < region->perm_count; i++) {
		if (region->perm_entries[i].app_id == app_id) {
			region->perm_entries[i].perm = perm;
			return 0;
		}
	}
	
	if (region->perm_count >= SHMEM_MAX_PERM_ENTRIES) {
		return -ENOMEM;
	}
	
	region->perm_entries[region->perm_count].app_id = app_id;
	region->perm_entries[region->perm_count].perm = perm;
	region->perm_count++;
	
	return 0;
}

int shmem_lock(shmem_handle_t handle, k_timeout_t timeout)
{
	struct shmem_region *region = get_region(handle);
	if (!region) {
		return -EINVAL;
	}
	
	int ret = k_mutex_lock(&region->lock, timeout);
	if (ret == 0) {
		region->is_locked = true;
		region->lock_owner = get_current_app_id();
	}
	
	return ret;
}

int shmem_unlock(shmem_handle_t handle)
{
	struct shmem_region *region = get_region(handle);
	if (!region) {
		return -EINVAL;
	}
	
	if (region->lock_owner != get_current_app_id()) {
		return -EACCES;
	}
	
	region->is_locked = false;
	return k_mutex_unlock(&region->lock);
}

ssize_t shmem_read(shmem_handle_t handle, size_t offset, void *data, size_t len)
{
	struct shmem_region *region = get_region(handle);
	if (!region || !data) {
		return -EINVAL;
	}
	
	if (!check_permission(region, get_current_app_id(), SHMEM_PERM_READ)) {
		return -EACCES;
	}
	
	if (offset >= region->size) {
		return 0;
	}
	
	size_t available = region->size - offset;
	size_t to_read = (len < available) ? len : available;
	
	memcpy(data, (uint8_t *)region->data + offset, to_read);
	
	return to_read;
}

ssize_t shmem_write(shmem_handle_t handle, size_t offset, const void *data, size_t len)
{
	struct shmem_region *region = get_region(handle);
	if (!region || !data) {
		return -EINVAL;
	}
	
	if (!check_permission(region, get_current_app_id(), SHMEM_PERM_WRITE)) {
		return -EACCES;
	}
	
	if (offset >= region->size) {
		return 0;
	}
	
	size_t available = region->size - offset;
	size_t to_write = (len < available) ? len : available;
	
	memcpy((uint8_t *)region->data + offset, data, to_write);
	
	return to_write;
}
