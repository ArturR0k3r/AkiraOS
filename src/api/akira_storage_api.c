/**
 * @file akira_storage_api.c
 * @brief Storage API implementation for WASM exports
 */

#include "akira_api.h"
#include "../storage/fs_manager.h"
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(akira_storage_api, LOG_LEVEL_INF);

// TODO: Add capability check before each API call 
// TODO: Implement per-app storage isolation 
// TODO: Add quota enforcement 
// TODO: Add encryption for sensitive data
// TODO: Add wear leveling statistics

#define APP_STORAGE_BASE "/lfs/apps"
#define MAX_PATH_LEN 128

// TODO: Get container name from OCRE context 
static const char *get_current_app_name(void)
{
    return "default_app";
}

static int sanitize_path(const char *path)
{
    if (!path || path[0] == '\0') {
        return -1;
    }
    
    // Reject absolute paths
    if (path[0] == '/') {
        LOG_ERR("Absolute paths not allowed: %s", path);
        return -2;
    }
    
    // Reject directory traversal attempts
    if (strstr(path, "..") != NULL) {
        LOG_ERR("Directory traversal attempted: %s", path);
        return -3;
    }
    
    // Check path length
    if (strlen(path) > MAX_PATH_LEN) {
        LOG_ERR("Path too long: %s", path);
        return -4;
    }
    
    return 0;
}

static int build_app_path(const char *path, char *out, size_t out_len)
{
    if (sanitize_path(path) < 0) {
        return -1;
    }

    const char *app = get_current_app_name();
    int ret = snprintf(out, out_len, "%s/%s/%s", APP_STORAGE_BASE, app, path);

    if (ret < 0 || ret >= (int)out_len) {
        LOG_ERR("Path buffer too small");
        return -2;
    }

    return 0;
}

static int ensure_app_dir_exists(const char *app_name)
{
    char app_dir[MAX_PATH_LEN];
    snprintf(app_dir, sizeof(app_dir), "%s/%s", APP_STORAGE_BASE, app_name);
    
    struct fs_dirent entry;
    int rc = fs_stat(app_dir, &entry);
    
    if (rc == 0) {
        return 0; // Directory exists
    }
    
    // Create directory
    rc = fs_mkdir(app_dir);
    if (rc < 0 && rc != -EEXIST) {
        LOG_ERR("Failed to create app directory: %d", rc);
        return rc;
    }
    
    return 0;
}

int akira_storage_read(const char *path, void *buffer, size_t len)
{
    // TODO: Check CAP_STORAGE_READ capability 

    if (!path || !buffer || len == 0) {
        return -EINVAL;
    }

    char full_path[MAX_PATH_LEN];
    if (build_app_path(path, full_path, sizeof(full_path)) < 0) {
        return -EINVAL;
    }

    LOG_DBG("storage_read: %s, max=%zu", full_path, len);

    struct fs_file_t file;
    fs_file_t_init(&file);
    
    int rc = fs_open(&file, full_path, FS_O_READ);
    if (rc < 0) {
        LOG_ERR("Failed to open file %s: %d", full_path, rc);
        return rc;
    }
    
    ssize_t bytes_read = fs_read(&file, buffer, len);
    fs_close(&file);
    
    if (bytes_read < 0) {
        LOG_ERR("Failed to read file %s: %d", full_path, (int)bytes_read);
        return (int)bytes_read;
    }
    
    LOG_DBG("Read %d bytes from %s", (int)bytes_read, path);
    return (int)bytes_read;
}

int akira_storage_write(const char *path, const void *data, size_t len)
{
    // TODO: Check CAP_STORAGE_WRITE capability 
    // TODO: Check quota before writing 

    if (!path || !data || len == 0) {
        return -EINVAL;
    }

    const char *app = get_current_app_name();
    ensure_app_dir_exists(app);

    char full_path[MAX_PATH_LEN];
    if (build_app_path(path, full_path, sizeof(full_path)) < 0) {
        return -EINVAL;
    }

    LOG_DBG("storage_write: %s, len=%zu", full_path, len);

    struct fs_file_t file;
    fs_file_t_init(&file);
    
    int rc = fs_open(&file, full_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (rc < 0) {
        LOG_ERR("Failed to open file %s for writing: %d", full_path, rc);
        return rc;
    }
    
    ssize_t bytes_written = fs_write(&file, data, len);
    fs_close(&file);
    
    if (bytes_written < 0) {
        LOG_ERR("Failed to write file %s: %d", full_path, (int)bytes_written);
        return (int)bytes_written;
    }
    
    LOG_INF("Wrote %d bytes to %s", (int)bytes_written, path);
    return (int)bytes_written;
}

int akira_storage_delete(const char *path)
{
    // TODO: Check CAP_STORAGE_WRITE capability 

    if (!path) {
        return -EINVAL;
    }

    char full_path[MAX_PATH_LEN];
    if (build_app_path(path, full_path, sizeof(full_path)) < 0) {
        return -EINVAL;
    }

    LOG_DBG("storage_delete: %s", full_path);

    int rc = fs_unlink(full_path);
    if (rc < 0) {
        LOG_ERR("Failed to delete file %s: %d", full_path, rc);
        return rc;
    }
    
    LOG_INF("Deleted file: %s", path);
    return 0;
}

int akira_storage_list(const char *path, char **files, int max_count)
{
    // TODO: Check CAP_STORAGE_READ capability 

    if (!path || !files || max_count <= 0) {
        return -EINVAL;
    }

    char full_path[MAX_PATH_LEN];
    if (build_app_path(path, full_path, sizeof(full_path)) < 0) {
        return -EINVAL;
    }

    LOG_DBG("storage_list: %s, max=%d", full_path, max_count);

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);
    
    int rc = fs_opendir(&dir, full_path);
    if (rc < 0) {
        LOG_ERR("Failed to open directory %s: %d", full_path, rc);
        return rc;
    }
    
    int count = 0;
    struct fs_dirent entry;
    
    while (count < max_count) {
        rc = fs_readdir(&dir, &entry);
        if (rc < 0) {
            break;
        }
        
        if (entry.name[0] == 0) {
            break; // End of directory
        }
        
        // Skip . and ..
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) {
            continue;
        }
        
        // Copy filename (files array must be pre-allocated)
        if (files[count] != NULL) {
            strncpy(files[count], entry.name, MAX_PATH_LEN - 1);
            files[count][MAX_PATH_LEN - 1] = '\0';
            count++;
        }
    }
    
    fs_closedir(&dir);
    
    LOG_DBG("Listed %d files in %s", count, path);
    return count;
}

int akira_storage_size(const char *path)
{
    // TODO: Check CAP_STORAGE_READ capability 

    if (!path) {
        return -EINVAL;
    }

    char full_path[MAX_PATH_LEN];
    if (build_app_path(path, full_path, sizeof(full_path)) < 0) {
        return -EINVAL;
    }

    struct fs_dirent entry;
    int rc = fs_stat(full_path, &entry);
    if (rc < 0) {
        LOG_ERR("Failed to stat file %s: %d", full_path, rc);
        return rc;
    }

    return (int)entry.size;
}

bool akira_storage_exists(const char *path)
{
    // TODO: Check CAP_STORAGE_READ capability 

    if (!path) {
        return false;
    }

    char full_path[MAX_PATH_LEN];
    if (build_app_path(path, full_path, sizeof(full_path)) < 0) {
        return false;
    }

    struct fs_dirent entry;
    int rc = fs_stat(full_path, &entry);
    return (rc == 0);
}

/*===========================================================================*/
/* WASM Wrappers (exported to OCRE)                                          */
/*===========================================================================*/

#include <wasm_export.h>

int akira_storage_read_wasm(wasm_exec_env_t exec_env, uint32_t path_ptr, uint32_t buf_ptr, int len)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    
    const char *path = (const char *)wasm_runtime_addr_app_to_native(module_inst, path_ptr);
    void *buf = (void *)wasm_runtime_addr_app_to_native(module_inst, buf_ptr);
    if (!path || !buf)
        return -1;
    
    return akira_storage_read(path, buf, (size_t)len);
}

int akira_storage_write_wasm(wasm_exec_env_t exec_env, uint32_t path_ptr, uint32_t data_ptr, int len)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    
    const char *path = (const char *)wasm_runtime_addr_app_to_native(module_inst, path_ptr);
    const void *data = (const void *)wasm_runtime_addr_app_to_native(module_inst, data_ptr);
    if (!path || !data)
        return -1;
    
    return akira_storage_write(path, data, (size_t)len);
}

int akira_storage_delete_wasm(wasm_exec_env_t exec_env, uint32_t path_ptr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    
    const char *path = (const char *)wasm_runtime_addr_app_to_native(module_inst, path_ptr);
    if (!path)
        return -1;
    
    return akira_storage_delete(path);
}

int akira_storage_size_wasm(wasm_exec_env_t exec_env, uint32_t path_ptr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    
    const char *path = (const char *)wasm_runtime_addr_app_to_native(module_inst, path_ptr);
    if (!path)
        return -1;
    
    return akira_storage_size(path);
}
