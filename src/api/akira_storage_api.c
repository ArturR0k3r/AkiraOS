/**
 * @file akira_storage_api.c
 * @brief Storage API implementation for WASM exports
 */

#include "akira_api.h"
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>

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

static int build_app_path(const char *path, char *out, size_t out_len)
{
    // TODO: Sanitize path to prevent directory traversal
    // TODO: Check path length limits

    const char *app = get_current_app_name();
    int ret = snprintf(out, out_len, "%s/%s/%s", APP_STORAGE_BASE, app, path);

    if (ret < 0 || ret >= (int)out_len)
    {
        return -1;
    }

    return 0;
}

int akira_storage_read(const char *path, void *buffer, size_t len)
{
    // TODO: Check CAP_STORAGE_READ capability
    // TODO: Implement actual file read using LittleFS

    if (!path || !buffer || len == 0)
    {
        return -1;
    }

    char full_path[MAX_PATH_LEN];
    if (build_app_path(path, full_path, sizeof(full_path)) < 0)
    {
        LOG_ERR("Path too long: %s", path);
        return -2;
    }

    LOG_DBG("storage_read: %s, max=%zu", full_path, len);

    // TODO: Open file, read data, close file
    // struct fs_file_t file;
    // fs_file_t_init(&file);
    // int rc = fs_open(&file, full_path, FS_O_READ);
    // ...

    return -1; // Not implemented
}

int akira_storage_write(const char *path, const void *data, size_t len)
{
    // TODO: Check CAP_STORAGE_WRITE capability
    // TODO: Check quota before writing
    // TODO: Create parent directories if needed

    if (!path || !data || len == 0)
    {
        return -1;
    }

    char full_path[MAX_PATH_LEN];
    if (build_app_path(path, full_path, sizeof(full_path)) < 0)
    {
        LOG_ERR("Path too long: %s", path);
        return -2;
    }

    LOG_DBG("storage_write: %s, len=%zu", full_path, len);

    // TODO: Open file (create), write data, close file
    // TODO: Update quota tracking

    return -1; // Not implemented
}

int akira_storage_delete(const char *path)
{
    // TODO: Check CAP_STORAGE_WRITE capability

    if (!path)
    {
        return -1;
    }

    char full_path[MAX_PATH_LEN];
    if (build_app_path(path, full_path, sizeof(full_path)) < 0)
    {
        return -2;
    }

    LOG_DBG("storage_delete: %s", full_path);

    // TODO: Delete file using fs_unlink()
    // TODO: Update quota tracking

    return -1; // Not implemented
}

int akira_storage_list(const char *path, char **files, int max_count)
{
    // TODO: Check CAP_STORAGE_READ capability

    if (!path || !files || max_count <= 0)
    {
        return -1;
    }

    char full_path[MAX_PATH_LEN];
    if (build_app_path(path, full_path, sizeof(full_path)) < 0)
    {
        return -2;
    }

    LOG_DBG("storage_list: %s, max=%d", full_path, max_count);

    // TODO: Open directory, iterate entries
    // struct fs_dir_t dir;
    // fs_dir_t_init(&dir);
    // fs_opendir(&dir, full_path);
    // ...

    return -1; // Not implemented
}

int akira_storage_size(const char *path)
{
    // TODO: Check CAP_STORAGE_READ capability

    if (!path)
    {
        return -1;
    }

    char full_path[MAX_PATH_LEN];
    if (build_app_path(path, full_path, sizeof(full_path)) < 0)
    {
        return -2;
    }

    // TODO: Get file info using fs_stat()
    // struct fs_dirent entry;
    // fs_stat(full_path, &entry);
    // return entry.size;

    return -1; // Not implemented
}
