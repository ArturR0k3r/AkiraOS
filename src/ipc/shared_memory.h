/**
 * @file shared_memory.h
 * @brief Shared Memory IPC
 *
 * Zero-copy data sharing between WASM apps with permission control.
 * Used for efficient large data transfers (images, audio, etc.)
 */

#ifndef AKIRA_SHARED_MEMORY_H
#define AKIRA_SHARED_MEMORY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Maximum shared memory regions
 */
#define SHMEM_MAX_REGIONS 16

/**
 * @brief Maximum name length
 */
#define SHMEM_MAX_NAME_LEN 32

	/**
	 * @brief Shared memory permissions
	 */
	typedef enum
	{
		SHMEM_PERM_NONE = 0x00,
		SHMEM_PERM_READ = 0x01,
		SHMEM_PERM_WRITE = 0x02,
		SHMEM_PERM_RW = 0x03
	} shmem_perm_t;

	/**
	 * @brief Shared memory region handle
	 */
	typedef int shmem_handle_t;

	/**
	 * @brief Shared memory region info
	 */
	struct shmem_info
	{
		char name[SHMEM_MAX_NAME_LEN];
		size_t size;
		uint32_t owner_id;
		uint32_t ref_count;
		shmem_perm_t default_perm;
	};

	/**
	 * @brief Initialize shared memory subsystem
	 * @return 0 on success
	 */
	int shmem_init(void);

	/**
	 * @brief Create shared memory region
	 * @param name Region name
	 * @param size Size in bytes
	 * @param default_perm Default permissions for other apps
	 * @return Handle or negative error
	 */
	shmem_handle_t shmem_create(const char *name, size_t size, shmem_perm_t default_perm);

	/**
	 * @brief Open existing shared memory region
	 * @param name Region name
	 * @param requested_perm Requested permissions
	 * @return Handle or negative error
	 */
	shmem_handle_t shmem_open(const char *name, shmem_perm_t requested_perm);

	/**
	 * @brief Close shared memory handle
	 * @param handle Region handle
	 * @return 0 on success
	 */
	int shmem_close(shmem_handle_t handle);

	/**
	 * @brief Destroy shared memory region (owner only)
	 * @param handle Region handle
	 * @return 0 on success
	 */
	int shmem_destroy(shmem_handle_t handle);

	/**
	 * @brief Map shared memory into address space
	 * @param handle Region handle
	 * @return Pointer to memory or NULL
	 */
	void *shmem_map(shmem_handle_t handle);

	/**
	 * @brief Unmap shared memory
	 * @param handle Region handle
	 * @return 0 on success
	 */
	int shmem_unmap(shmem_handle_t handle);

	/**
	 * @brief Get region info
	 * @param handle Region handle
	 * @param info Output for region info
	 * @return 0 on success
	 */
	int shmem_get_info(shmem_handle_t handle, struct shmem_info *info);

	/**
	 * @brief Set app permissions for region
	 * @param handle Region handle
	 * @param app_id Target app ID
	 * @param perm Permissions to grant
	 * @return 0 on success
	 */
	int shmem_set_permission(shmem_handle_t handle, uint32_t app_id, shmem_perm_t perm);

	/**
	 * @brief Lock region for exclusive access
	 * @param handle Region handle
	 * @param timeout Lock timeout
	 * @return 0 on success
	 */
	int shmem_lock(shmem_handle_t handle, k_timeout_t timeout);

	/**
	 * @brief Unlock region
	 * @param handle Region handle
	 * @return 0 on success
	 */
	int shmem_unlock(shmem_handle_t handle);

	/**
	 * @brief Read from shared memory
	 * @param handle Region handle
	 * @param offset Offset in region
	 * @param data Output buffer
	 * @param len Bytes to read
	 * @return Bytes read or negative error
	 */
	ssize_t shmem_read(shmem_handle_t handle, size_t offset, void *data, size_t len);

	/**
	 * @brief Write to shared memory
	 * @param handle Region handle
	 * @param offset Offset in region
	 * @param data Input buffer
	 * @param len Bytes to write
	 * @return Bytes written or negative error
	 */
	ssize_t shmem_write(shmem_handle_t handle, size_t offset, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SHARED_MEMORY_H */
