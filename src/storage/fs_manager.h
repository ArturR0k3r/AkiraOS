/**
 * @file fs_manager.h
 * @brief AkiraOS Filesystem Manager
 *
 * Unified filesystem interface supporting:
 * - SD card (FAT32)
 * - Internal flash (LittleFS) 
 * - RAM disk (for temporary storage)
 * - Automatic fallback and path resolution
 */

#ifndef AKIRA_FS_MANAGER_H
#define AKIRA_FS_MANAGER_H

#include <zephyr/fs/fs.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Filesystem types */
typedef enum {
    FS_TYPE_SD_CARD,      /* /SD: - FAT32 */
    FS_TYPE_INTERNAL,     /* /data: - LittleFS or RAM */
    FS_TYPE_TEMPORARY,    /* /ram: - RAM disk */
    FS_TYPE_UNKNOWN
} fs_type_t;

/* Storage info */
typedef struct {
    fs_type_t type;
    const char *mount_point;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t used_bytes;
    bool available;
    bool writable;
} fs_info_t;

/* App storage context */
typedef struct {
    char app_name[64];
    char storage_path[256];
    fs_type_t storage_type;
    size_t current_size;
    size_t max_size;
} app_storage_ctx_t;

/**
 * Initialize filesystem manager
 * @return 0 on success, negative error code otherwise
 */
int fs_manager_init(void);

/**
 * Get available filesystems and their status
 * @param info - Array to store filesystem info
 * @param max_count - Maximum number of entries
 * @return Number of filesystems found
 */
int fs_manager_get_info(fs_info_t *info, size_t max_count);

/**
 * Get info for specific filesystem
 * @param type - Filesystem type
 * @param info - Output filesystem info
 * @return 0 on success, negative error code otherwise
 */
int fs_manager_get_type_info(fs_type_t type, fs_info_t *info);

/**
 * Create directory (creates all parent directories)
 * @param path - Directory path
 * @return 0 on success, negative error code otherwise
 */
int fs_manager_mkdir(const char *path);

/**
 * Write file (creates or truncates)
 * @param path - File path
 * @param data - Data to write
 * @param size - Data size
 * @return Bytes written on success, negative error code otherwise
 */
ssize_t fs_manager_write_file(const char *path, const void *data, size_t size);

/**
 * Read file
 * @param path - File path
 * @param buffer - Output buffer
 * @param max_size - Maximum bytes to read
 * @return Bytes read on success, negative error code otherwise
 */
ssize_t fs_manager_read_file(const char *path, void *buffer, size_t max_size);

/**
 * Append to file
 * @param path - File path
 * @param data - Data to append
 * @param size - Data size
 * @return Bytes written on success, negative error code otherwise
 */
ssize_t fs_manager_append_file(const char *path, const void *data, size_t size);

/**
 * Delete file
 * @param path - File path
 * @return 0 on success, negative error code otherwise
 */
int fs_manager_delete_file(const char *path);

/**
 * Delete directory recursively
 * @param path - Directory path
 * @return 0 on success, negative error code otherwise
 */
int fs_manager_delete_dir(const char *path);

/**
 * Check if path exists
 * @param path - File or directory path
 * @return 1 if exists, 0 if not, negative error code on error
 */
int fs_manager_exists(const char *path);

/**
 * Get file size
 * @param path - File path
 * @return File size on success, negative error code otherwise
 */
ssize_t fs_manager_get_size(const char *path);

/**
 * Allocate app storage
 * @param app_name - Application name
 * @param max_size - Maximum size needed
 * @param ctx - Output storage context
 * @return 0 on success, negative error code otherwise
 */
int fs_manager_alloc_app_storage(const char *app_name, size_t max_size, app_storage_ctx_t *ctx);

/**
 * Free app storage
 * @param ctx - Storage context from alloc
 * @return 0 on success, negative error code otherwise
 */
int fs_manager_free_app_storage(app_storage_ctx_t *ctx);

/**
 * Write app data
 * @param ctx - Storage context
 * @param data - Data to write
 * @param size - Data size
 * @return Bytes written on success, negative error code otherwise
 */
ssize_t fs_manager_write_app_data(app_storage_ctx_t *ctx, const void *data, size_t size);

/**
 * Read app data
 * @param ctx - Storage context
 * @param buffer - Output buffer
 * @param max_size - Maximum bytes to read
 * @return Bytes read on success, negative error code otherwise
 */
ssize_t fs_manager_read_app_data(app_storage_ctx_t *ctx, void *buffer, size_t max_size);

/**
 * Format filesystem (dangerous - use with caution)
 * @param type - Filesystem type to format
 * @return 0 on success, negative error code otherwise
 */
int fs_manager_format(fs_type_t type);

/**
 * Get recommended storage path for content type
 * @param content_type - Type of content (app, data, cache, etc)
 * @param buffer - Output path buffer
 * @param max_len - Buffer size
 * @return 0 on success, negative error code otherwise
 */
int fs_manager_get_recommended_path(const char *content_type, char *buffer, size_t max_len);

/**
 * Get current storage status string
 * @return Status string (e.g., "SD Card", "Flash", "RAM Only")
 */
const char *fs_manager_get_status(void);

/**
 * Check if persistent storage is available
 * @return true if SD or flash storage available
 */
bool fs_manager_has_persistent_storage(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_FS_MANAGER_H */
