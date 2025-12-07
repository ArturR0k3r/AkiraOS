/**
 * @file fs_manager.c
 * @brief AkiraOS Filesystem Manager Implementation
 * 
 * Provides unified filesystem access for AkiraOS with support for:
 * - Internal flash (LittleFS)
 * - SD card (FAT)
 * - RAM-based temporary storage
 */

#include "fs_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(fs_manager, CONFIG_AKIRA_LOG_LEVEL);

/* Storage state */
static struct {
    bool initialized;
    bool sd_available;
    bool internal_available;
    bool ram_initialized;
} fs_state = {0};

/* Simple RAM-based file storage for fallback */
#define RAM_FILE_MAX_COUNT 16
#define RAM_FILE_MAX_SIZE  (64 * 1024)  /* 64KB per file */
#define RAM_FILE_NAME_MAX  128

typedef struct {
    char name[RAM_FILE_NAME_MAX];
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool in_use;
} ram_file_t;

static ram_file_t ram_files[RAM_FILE_MAX_COUNT];
static K_MUTEX_DEFINE(ram_mutex);

/* RAM file operations */
static ram_file_t *ram_find_file(const char *path) {
    for (int i = 0; i < RAM_FILE_MAX_COUNT; i++) {
        if (ram_files[i].in_use && strcmp(ram_files[i].name, path) == 0) {
            return &ram_files[i];
        }
    }
    return NULL;
}

static ram_file_t *ram_create_file(const char *path) {
    /* Check if already exists */
    ram_file_t *existing = ram_find_file(path);
    if (existing) {
        return existing;
    }
    
    /* Find free slot */
    for (int i = 0; i < RAM_FILE_MAX_COUNT; i++) {
        if (!ram_files[i].in_use) {
            strncpy(ram_files[i].name, path, RAM_FILE_NAME_MAX - 1);
            ram_files[i].name[RAM_FILE_NAME_MAX - 1] = '\0';
            ram_files[i].data = NULL;
            ram_files[i].size = 0;
            ram_files[i].capacity = 0;
            ram_files[i].in_use = true;
            return &ram_files[i];
        }
    }
    return NULL;
}

static int ram_write_file(const char *path, const void *data, size_t size) {
    k_mutex_lock(&ram_mutex, K_FOREVER);
    
    ram_file_t *file = ram_find_file(path);
    if (!file) {
        file = ram_create_file(path);
        if (!file) {
            k_mutex_unlock(&ram_mutex);
            return -ENOMEM;
        }
    }
    
    /* Allocate/reallocate buffer if needed */
    if (file->capacity < size) {
        if (file->data) {
            k_free(file->data);
        }
        file->data = k_malloc(size);
        if (!file->data) {
            file->in_use = false;
            k_mutex_unlock(&ram_mutex);
            return -ENOMEM;
        }
        file->capacity = size;
    }
    
    memcpy(file->data, data, size);
    file->size = size;
    
    k_mutex_unlock(&ram_mutex);
    return size;
}

static ssize_t ram_read_file(const char *path, void *buffer, size_t max_size) {
    k_mutex_lock(&ram_mutex, K_FOREVER);
    
    ram_file_t *file = ram_find_file(path);
    if (!file || !file->data) {
        k_mutex_unlock(&ram_mutex);
        return -ENOENT;
    }
    
    size_t to_read = (file->size < max_size) ? file->size : max_size;
    memcpy(buffer, file->data, to_read);
    
    k_mutex_unlock(&ram_mutex);
    return to_read;
}

static int ram_delete_file(const char *path) {
    k_mutex_lock(&ram_mutex, K_FOREVER);
    
    ram_file_t *file = ram_find_file(path);
    if (!file) {
        k_mutex_unlock(&ram_mutex);
        return -ENOENT;
    }
    
    if (file->data) {
        k_free(file->data);
    }
    memset(file, 0, sizeof(*file));
    
    k_mutex_unlock(&ram_mutex);
    return 0;
}

static bool ram_file_exists(const char *path) {
    k_mutex_lock(&ram_mutex, K_FOREVER);
    ram_file_t *file = ram_find_file(path);
    k_mutex_unlock(&ram_mutex);
    return file != NULL;
}

static ssize_t ram_file_size(const char *path) {
    k_mutex_lock(&ram_mutex, K_FOREVER);
    ram_file_t *file = ram_find_file(path);
    ssize_t size = file ? (ssize_t)file->size : -ENOENT;
    k_mutex_unlock(&ram_mutex);
    return size;
}

/* Check if path is for RAM storage */
static bool is_ram_path(const char *path) {
    return (strncmp(path, "/ram/", 5) == 0) || 
           (strncmp(path, "/tmp/", 5) == 0) ||
           (!fs_state.internal_available && !fs_state.sd_available);
}

/* Try to initialize internal flash storage */
static int init_internal_storage(void) {
    LOG_INF("Checking internal flash storage...");
    
    fs_state.internal_available = false;
    
    /* Try to check if /data mount point exists */
    struct fs_dirent entry;
    int ret = fs_stat("/data", &entry);
    if (ret == 0) {
        LOG_INF("Internal storage available at /data");
        fs_state.internal_available = true;
        
        /* Create app directories */
        fs_mkdir("/data/apps");
        fs_mkdir("/data/app_data");
        return 0;
    }
    
    LOG_DBG("No internal flash storage at /data: %d", ret);
    return -ENODEV;
}

/* Try to initialize SD card */
static int init_sd_storage(void) {
    LOG_INF("Checking SD card storage...");
    
    struct fs_dirent entry;
    int ret = fs_stat("/SD:", &entry);
    if (ret == 0) {
        LOG_INF("SD card available at /SD:");
        fs_state.sd_available = true;
        
        /* Create app directories on SD */
        fs_mkdir("/SD:/apps");
        fs_mkdir("/SD:/app_data");
        return 0;
    }
    
    LOG_DBG("SD card not available: %d", ret);
    return -ENODEV;
}

/**
 * Initialize filesystem manager
 */
int fs_manager_init(void) {
    if (fs_state.initialized) {
        return 0;
    }

    LOG_INF("Initializing AkiraOS Filesystem Manager");

    /* Initialize RAM file system */
    memset(ram_files, 0, sizeof(ram_files));
    fs_state.ram_initialized = true;
    
    /* Try internal flash storage */
    init_internal_storage();
    
    /* Try SD card */
    init_sd_storage();
    
    /* Log available storage */
    if (fs_state.internal_available) {
        LOG_INF("✅ Internal flash storage ready");
    }
    if (fs_state.sd_available) {
        LOG_INF("✅ SD card storage ready");
    }
    if (!fs_state.internal_available && !fs_state.sd_available) {
        LOG_WRN("⚠️ Using RAM-only storage (not persistent!)");
    }

    fs_state.initialized = true;
    LOG_INF("Filesystem Manager initialized");

    return 0;
}

/**
 * Get available filesystems
 */
int fs_manager_get_info(fs_info_t *info, size_t max_count) {
    if (!info || max_count == 0) {
        return 0;
    }

    int count = 0;

    /* Internal storage */
    if (fs_state.internal_available && count < max_count) {
        struct fs_statvfs stat;
        if (fs_statvfs("/data", &stat) == 0) {
            info[count].type = FS_TYPE_INTERNAL;
            info[count].mount_point = "/data";
            info[count].total_bytes = stat.f_frsize * stat.f_blocks;
            info[count].free_bytes = stat.f_frsize * stat.f_bfree;
            info[count].used_bytes = info[count].total_bytes - info[count].free_bytes;
            info[count].available = true;
            info[count].writable = true;
            count++;
        }
    }

    /* SD card */
    if (fs_state.sd_available && count < max_count) {
        struct fs_statvfs stat;
        if (fs_statvfs("/SD:", &stat) == 0) {
            info[count].type = FS_TYPE_SD_CARD;
            info[count].mount_point = "/SD:";
            info[count].total_bytes = stat.f_frsize * stat.f_blocks;
            info[count].free_bytes = stat.f_frsize * stat.f_bfree;
            info[count].used_bytes = info[count].total_bytes - info[count].free_bytes;
            info[count].available = true;
            info[count].writable = true;
            count++;
        }
    }

    /* RAM storage (always available) */
    if (count < max_count) {
        info[count].type = FS_TYPE_TEMPORARY;
        info[count].mount_point = "/ram";
        info[count].total_bytes = RAM_FILE_MAX_COUNT * RAM_FILE_MAX_SIZE;
        info[count].free_bytes = info[count].total_bytes; /* Simplified */
        info[count].used_bytes = 0;
        info[count].available = true;
        info[count].writable = true;
        count++;
    }

    return count;
}

/**
 * Get info for specific filesystem
 */
int fs_manager_get_type_info(fs_type_t type, fs_info_t *info) {
    if (!info) {
        return -EINVAL;
    }

    fs_info_t temp_info[3];
    int count = fs_manager_get_info(temp_info, 3);

    for (int i = 0; i < count; i++) {
        if (temp_info[i].type == type) {
            memcpy(info, &temp_info[i], sizeof(*info));
            return 0;
        }
    }

    return -ENODEV;
}

/**
 * Create directory
 */
int fs_manager_mkdir(const char *path) {
    if (!path || !fs_state.initialized) {
        return -EINVAL;
    }

    /* RAM paths don't need directories */
    if (is_ram_path(path)) {
        return 0;
    }

    int ret = fs_mkdir(path);
    if (ret < 0 && ret != -EEXIST) {
        LOG_DBG("mkdir %s failed: %d", path, ret);
        return ret;
    }

    return 0;
}

/**
 * Write file
 */
ssize_t fs_manager_write_file(const char *path, const void *data, size_t size) {
    if (!path || !data || !fs_state.initialized) {
        return -EINVAL;
    }

    /* Use RAM storage if no persistent storage or RAM path */
    if (is_ram_path(path)) {
        return ram_write_file(path, data, size);
    }

    /* Ensure parent directory exists */
    char parent[256];
    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char *last_slash = strrchr(parent, '/');
    if (last_slash && last_slash != parent) {
        *last_slash = '\0';
        fs_manager_mkdir(parent);
    }

    /* Open and write file */
    struct fs_file_t file;
    fs_file_t_init(&file);

    int ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
    if (ret < 0) {
        LOG_ERR("Failed to open %s for writing: %d", path, ret);
        /* Fall back to RAM */
        return ram_write_file(path, data, size);
    }

    /* Truncate file */
    fs_truncate(&file, 0);
    fs_seek(&file, 0, FS_SEEK_SET);

    ssize_t written = fs_write(&file, data, size);
    fs_close(&file);

    if (written < 0) {
        LOG_ERR("Failed to write to %s: %zd", path, written);
        /* Fall back to RAM */
        return ram_write_file(path, data, size);
    }

    return written;
}

/**
 * Read file
 */
ssize_t fs_manager_read_file(const char *path, void *buffer, size_t max_size) {
    if (!path || !buffer || !fs_state.initialized) {
        return -EINVAL;
    }

    /* Check RAM storage first if RAM path or no persistent storage */
    if (is_ram_path(path)) {
        return ram_read_file(path, buffer, max_size);
    }

    struct fs_file_t file;
    fs_file_t_init(&file);

    int ret = fs_open(&file, path, FS_O_READ);
    if (ret < 0) {
        /* Try RAM fallback */
        return ram_read_file(path, buffer, max_size);
    }

    ssize_t read_bytes = fs_read(&file, buffer, max_size);
    fs_close(&file);

    if (read_bytes < 0) {
        LOG_DBG("Failed to read from %s: %zd", path, read_bytes);
    }

    return read_bytes;
}

/**
 * Append to file
 */
ssize_t fs_manager_append_file(const char *path, const void *data, size_t size) {
    if (!path || !data || !fs_state.initialized) {
        return -EINVAL;
    }

    /* For RAM storage, we need to read + write */
    if (is_ram_path(path)) {
        /* Simple implementation: read existing, append, write back */
        uint8_t *temp = k_malloc(RAM_FILE_MAX_SIZE);
        if (!temp) {
            return -ENOMEM;
        }
        
        ssize_t existing = ram_read_file(path, temp, RAM_FILE_MAX_SIZE - size);
        if (existing < 0) {
            existing = 0;
        }
        
        if (existing + size > RAM_FILE_MAX_SIZE) {
            k_free(temp);
            return -ENOSPC;
        }
        
        memcpy(temp + existing, data, size);
        ssize_t ret = ram_write_file(path, temp, existing + size);
        k_free(temp);
        return ret < 0 ? ret : (ssize_t)size;
    }

    struct fs_file_t file;
    fs_file_t_init(&file);

    int ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
    if (ret < 0) {
        LOG_ERR("Failed to open %s for appending: %d", path, ret);
        return ret;
    }

    ssize_t written = fs_write(&file, data, size);
    fs_close(&file);

    if (written < 0) {
        LOG_ERR("Failed to append to %s: %zd", path, written);
    }

    return written;
}

/**
 * Delete file
 */
int fs_manager_delete_file(const char *path) {
    if (!path || !fs_state.initialized) {
        return -EINVAL;
    }

    if (is_ram_path(path)) {
        return ram_delete_file(path);
    }

    int ret = fs_unlink(path);
    if (ret < 0 && ret != -ENOENT) {
        LOG_DBG("Failed to delete %s: %d", path, ret);
    }

    /* Also try to delete from RAM */
    ram_delete_file(path);

    return ret;
}

/**
 * Delete directory
 */
int fs_manager_delete_dir(const char *path) {
    if (!path || !fs_state.initialized) {
        return -EINVAL;
    }

    if (is_ram_path(path)) {
        return 0;  /* RAM doesn't have directories */
    }

    /* Note: This only removes empty directories */
    int ret = fs_unlink(path);
    if (ret < 0 && ret != -ENOENT) {
        LOG_DBG("Failed to delete directory %s: %d", path, ret);
    }

    return ret;
}

/**
 * Check if path exists
 */
int fs_manager_exists(const char *path) {
    if (!path || !fs_state.initialized) {
        return -EINVAL;
    }

    if (is_ram_path(path)) {
        return ram_file_exists(path) ? 1 : 0;
    }

    struct fs_dirent entry;
    int ret = fs_stat(path, &entry);

    if (ret == 0) {
        return 1;  /* Exists */
    }

    /* Check RAM fallback */
    if (ram_file_exists(path)) {
        return 1;
    }

    if (ret == -ENOENT) {
        return 0;  /* Doesn't exist */
    }

    return ret;  /* Error */
}

/**
 * Get file size
 */
ssize_t fs_manager_get_size(const char *path) {
    if (!path || !fs_state.initialized) {
        return -EINVAL;
    }

    if (is_ram_path(path)) {
        return ram_file_size(path);
    }

    struct fs_dirent entry;
    int ret = fs_stat(path, &entry);

    if (ret < 0) {
        /* Try RAM fallback */
        ssize_t ram_size = ram_file_size(path);
        if (ram_size >= 0) {
            return ram_size;
        }
        return ret;
    }

    return entry.size;
}

/**
 * Allocate app storage
 */
int fs_manager_alloc_app_storage(const char *app_name, size_t max_size, app_storage_ctx_t *ctx) {
    if (!app_name || !ctx || !fs_state.initialized) {
        return -EINVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->app_name, app_name, sizeof(ctx->app_name) - 1);
    ctx->max_size = max_size;

    /* Choose best available storage */
    if (fs_state.sd_available) {
        snprintf(ctx->storage_path, sizeof(ctx->storage_path), "/SD:/apps/%s", app_name);
        ctx->storage_type = FS_TYPE_SD_CARD;
        fs_manager_mkdir("/SD:/apps");
    } else if (fs_state.internal_available) {
        snprintf(ctx->storage_path, sizeof(ctx->storage_path), "/data/apps/%s", app_name);
        ctx->storage_type = FS_TYPE_INTERNAL;
        fs_manager_mkdir("/data/apps");
    } else {
        /* Use RAM storage */
        snprintf(ctx->storage_path, sizeof(ctx->storage_path), "/ram/apps/%s", app_name);
        ctx->storage_type = FS_TYPE_TEMPORARY;
        LOG_WRN("Using RAM storage for %s (not persistent!)", app_name);
    }

    return 0;
}

/**
 * Free app storage
 */
int fs_manager_free_app_storage(app_storage_ctx_t *ctx) {
    if (!ctx) {
        return -EINVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    return 0;
}

/**
 * Write app data
 */
ssize_t fs_manager_write_app_data(app_storage_ctx_t *ctx, const void *data, size_t size) {
    if (!ctx || !data) {
        return -EINVAL;
    }

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s.wasm", ctx->storage_path);

    ssize_t ret = fs_manager_write_file(full_path, data, size);
    if (ret > 0) {
        ctx->current_size = ret;
    }

    return ret;
}

/**
 * Read app data
 */
ssize_t fs_manager_read_app_data(app_storage_ctx_t *ctx, void *buffer, size_t max_size) {
    if (!ctx || !buffer) {
        return -EINVAL;
    }

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s.wasm", ctx->storage_path);

    return fs_manager_read_file(full_path, buffer, max_size);
}

/**
 * Format filesystem
 */
int fs_manager_format(fs_type_t type) {
    LOG_WRN("Format requested for type %d - not implemented", type);
    return -ENOTSUP;
}

/**
 * Get recommended path for content type
 */
int fs_manager_get_recommended_path(const char *content_type, char *buffer, size_t max_len) {
    if (!content_type || !buffer || max_len == 0) {
        return -EINVAL;
    }

    const char *base;
    
    /* Choose best available storage */
    if (fs_state.sd_available) {
        base = "/SD:";
    } else if (fs_state.internal_available) {
        base = "/data";
    } else {
        base = "/ram";
    }

    if (strcmp(content_type, "app") == 0) {
        snprintf(buffer, max_len, "%s/apps", base);
    } else if (strcmp(content_type, "data") == 0) {
        snprintf(buffer, max_len, "%s/app_data", base);
    } else if (strcmp(content_type, "cache") == 0) {
        snprintf(buffer, max_len, "%s/cache", base);
    } else if (strcmp(content_type, "log") == 0) {
        snprintf(buffer, max_len, "%s/logs", base);
    } else {
        strncpy(buffer, base, max_len - 1);
        buffer[max_len - 1] = '\0';
    }

    return 0;
}

/**
 * Get current storage status string
 */
const char *fs_manager_get_status(void) {
    if (fs_state.sd_available && fs_state.internal_available) {
        return "SD+Flash";
    } else if (fs_state.sd_available) {
        return "SD Card";
    } else if (fs_state.internal_available) {
        return "Flash";
    } else {
        return "RAM Only";
    }
}

/**
 * Check if persistent storage is available
 */
bool fs_manager_has_persistent_storage(void) {
    return fs_state.sd_available || fs_state.internal_available;
}

/**
 * List files in RAM storage
 */
int fs_manager_list_ram_files(ram_file_info_t *info, size_t max_count) {
    if (!info || max_count == 0) {
        return -EINVAL;
    }

    k_mutex_lock(&ram_mutex, K_FOREVER);
    
    int count = 0;
    for (int i = 0; i < RAM_FILE_MAX_COUNT && count < max_count; i++) {
        if (ram_files[i].in_use) {
            info[count].path = ram_files[i].name;
            info[count].size = ram_files[i].size;
            count++;
        }
    }
    
    k_mutex_unlock(&ram_mutex);
    return count;
}

/**
 * Get count of files in RAM storage
 */
int fs_manager_get_ram_file_count(void) {
    k_mutex_lock(&ram_mutex, K_FOREVER);
    
    int count = 0;
    for (int i = 0; i < RAM_FILE_MAX_COUNT; i++) {
        if (ram_files[i].in_use) {
            count++;
        }
    }
    
    k_mutex_unlock(&ram_mutex);
    return count;
}
