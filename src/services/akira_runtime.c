/**
 * @file akira_runtime.c
 * @brief AkiraOS Runtime Implementation
 *
 * Thin wrapper around OCRE container runtime.
 * Uses container_id directly (no name-based lookups).
 */

#include "akira_runtime.h"
#include "../storage/fs_manager.h"

#include <ocre/ocre.h>
#include <ocre/ocre_container_runtime/ocre_container_runtime.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(akira_runtime, CONFIG_AKIRA_LOG_LEVEL);

/* ===== Configuration ===== */

/* OCRE's expected image path (from core_fs.c) */
#define OCRE_IMAGE_PATH "/lfs/ocre/images"
#define MAX_PATH_LEN 64

/* ===== Static State ===== */

static ocre_cs_ctx g_ctx;
static bool g_initialized = false;

/* ===== Initialization ===== */

int akira_runtime_init(void)
{
    if (g_initialized) {
        LOG_WRN("Akira runtime already initialized");
        return 0;
    }

    LOG_INF("Initializing Akira runtime...");

    /* Initialize OCRE storage (creates /lfs/ocre/images directory) */
    /* Note: On Zephyr, the function is called ocre_app_storage_partition_init */
#ifdef CONFIG_FILE_SYSTEM_LITTLEFS
    extern void ocre_app_storage_partition_init(void);
    ocre_app_storage_partition_init();
#endif

    /* Initialize OCRE container runtime */
    ocre_container_init_arguments_t args = {0};
    ocre_container_runtime_status_t status = ocre_container_runtime_init(&g_ctx, &args);

    if (status != RUNTIME_STATUS_INITIALIZED) {
        LOG_ERR("Failed to initialize OCRE runtime: %d", status);
        return -EIO;
    }

    g_initialized = true;
    LOG_INF("Akira runtime initialized successfully");
    return 0;
}

bool akira_runtime_is_initialized(void)
{
    return g_initialized;
}

/* ===== Binary Management ===== */

int akira_runtime_save_binary(const char *name, const void *binary, size_t size)
{
    if (!name || !binary || size == 0) {
        return -EINVAL;
    }

    /* Construct path: /lfs/ocre/images/{name}.bin */
    char path[MAX_PATH_LEN];
    int ret = snprintf(path, sizeof(path), "%s/%s.bin", OCRE_IMAGE_PATH, name);
    if (ret < 0 || ret >= sizeof(path)) {
        LOG_ERR("Path too long for app: %s", name);
        return -ENAMETOOLONG;
    }

    /* Ensure directory exists */
    struct fs_dirent entry;
    if (fs_stat(OCRE_IMAGE_PATH, &entry) != 0) {
        LOG_INF("Creating OCRE images directory: %s", OCRE_IMAGE_PATH);
        /* Create parent dirs */
        fs_mkdir("/lfs");
        fs_mkdir("/lfs/ocre");
        fs_mkdir(OCRE_IMAGE_PATH);
    }

    /* Try filesystem first, fall back to RAM storage via fs_manager */
    struct fs_file_t file;
    fs_file_t_init(&file);
    
    ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
    if (ret == 0) {
        /* Write to real filesystem */
        ssize_t written = fs_write(&file, binary, size);
        fs_close(&file);
        
        if (written == size) {
            LOG_INF("Saved binary to filesystem: %s (%zu bytes)", path, size);
            return 0;
        }
        LOG_WRN("Filesystem write incomplete: %zd/%zu", written, size);
        fs_unlink(path);  /* Clean up partial write */
    } else {
        LOG_WRN("Filesystem not available (err %d), using RAM fallback", ret);
    }

    /* Fallback to fs_manager (RAM storage) */
    ret = fs_manager_write_file(path, binary, size);
    if (ret < 0) {
        LOG_ERR("Failed to save binary: %d", ret);
        return ret;
    }

    LOG_INF("Saved binary to RAM storage: %s (%zu bytes)", path, size);
    return 0;
}

int akira_runtime_delete_binary(const char *name)
{
    if (!name) {
        return -EINVAL;
    }

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s.bin", OCRE_IMAGE_PATH, name);

    /* Try filesystem first */
    int ret = fs_unlink(path);
    if (ret == 0) {
        LOG_INF("Deleted binary from filesystem: %s", path);
        return 0;
    }

    /* Try fs_manager (RAM storage) */
    ret = fs_manager_delete_file(path);
    if (ret == 0) {
        LOG_INF("Deleted binary from RAM storage: %s", path);
        return 0;
    }

    LOG_WRN("Binary not found or delete failed: %s", path);
    return ret;
}

/* ===== Container Operations ===== */

int akira_runtime_install(const char *name, const void *binary, size_t size)
{
    if (!g_initialized) {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    if (!name || !binary || size == 0) {
        return -EINVAL;
    }

    /* Save binary to OCRE's expected path */
    int ret = akira_runtime_save_binary(name, binary, size);
    if (ret < 0) {
        LOG_ERR("Failed to save binary for %s: %d", name, ret);
        return ret;
    }

    /* Prepare container data */
    ocre_container_data_t container_data = {0};
    strncpy(container_data.name, name, OCRE_MODULE_NAME_LEN - 1);
    strncpy(container_data.sha256, name, OCRE_SHA256_LEN - 1);  /* sha256 is used as filename */
    container_data.heap_size = 0;   /* Use defaults */
    container_data.stack_size = 0;  /* Use defaults */
    container_data.timers = 0;
    container_data.watchdog_interval = 0;

    /* Create container in OCRE */
    int container_id = -1;
    ocre_container_status_t status = ocre_container_runtime_create_container(
        &g_ctx, &container_data, &container_id, NULL);

    if (status == CONTAINER_STATUS_CREATED || status == CONTAINER_STATUS_UNKNOWN) {
        /* CONTAINER_STATUS_UNKNOWN is acceptable because creation is async */
        LOG_INF("Container created: %s (ID: %d)", name, container_id);
        return container_id;
    }

    LOG_ERR("Failed to create container %s: status=%d", name, status);
    akira_runtime_delete_binary(name);  /* Cleanup on failure */
    return -EIO;
}

int akira_runtime_start(int container_id)
{
    if (!g_initialized) {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    if (container_id < 0 || container_id >= CONFIG_MAX_CONTAINERS) {
        LOG_ERR("Invalid container ID: %d", container_id);
        return -EINVAL;
    }

    LOG_INF("Starting container %d...", container_id);
    
    ocre_container_status_t status = ocre_container_runtime_run_container(container_id, NULL);

    if (status != CONTAINER_STATUS_RUNNING) {
        LOG_ERR("Failed to start container %d: status=%d", container_id, status);
        return -EIO;
    }

    LOG_INF("Container %d start event sent", container_id);
    
    /* Wait briefly for container to start, checking status periodically */
    for (int i = 0; i < 10; i++) {
        k_sleep(K_MSEC(50));
        
        ocre_container_status_t actual = ocre_container_runtime_get_container_status(&g_ctx, container_id);
        
        if (actual == CONTAINER_STATUS_RUNNING) {
            LOG_INF("Container %d is running", container_id);
            return 0;
        } else if (actual == CONTAINER_STATUS_STOPPED) {
            /* Container ran and finished successfully */
            LOG_INF("Container %d completed execution", container_id);
            return 0;
        } else if (actual == CONTAINER_STATUS_ERROR) {
            LOG_ERR("Container %d failed with error", container_id);
            return -EIO;
        }
        /* Still CREATED - keep waiting */
    }
    
    /* Timeout - check final state */
    ocre_container_status_t final = ocre_container_runtime_get_container_status(&g_ctx, container_id);
    if (final == CONTAINER_STATUS_CREATED) {
        LOG_ERR("Container %d failed to start (instantiation error)", container_id);
        return -ENOMEM;
    }
    
    LOG_WRN("Container %d in state %d after timeout", container_id, final);
    return (final == CONTAINER_STATUS_RUNNING || final == CONTAINER_STATUS_STOPPED) ? 0 : -EIO;
}

int akira_runtime_stop(int container_id)
{
    if (!g_initialized) {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    if (container_id < 0 || container_id >= CONFIG_MAX_CONTAINERS) {
        LOG_ERR("Invalid container ID: %d", container_id);
        return -EINVAL;
    }

    LOG_INF("Stopping container %d...", container_id);

    ocre_container_status_t status = ocre_container_runtime_stop_container(container_id, NULL);

    if (status == CONTAINER_STATUS_STOPPED) {
        LOG_INF("Container %d stopped", container_id);
        return 0;
    }

    LOG_ERR("Failed to stop container %d: status=%d", container_id, status);
    return -EIO;
}

int akira_runtime_destroy(int container_id)
{
    if (!g_initialized) {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    if (container_id < 0 || container_id >= CONFIG_MAX_CONTAINERS) {
        LOG_ERR("Invalid container ID: %d", container_id);
        return -EINVAL;
    }

    LOG_INF("Destroying container %d...", container_id);

    ocre_container_status_t status = ocre_container_runtime_destroy_container(
        &g_ctx, container_id, NULL);

    if (status == CONTAINER_STATUS_DESTROYED) {
        LOG_INF("Container %d destroyed", container_id);
        return 0;
    }

    LOG_ERR("Failed to destroy container %d: status=%d", container_id, status);
    return -EIO;
}

int akira_runtime_uninstall(const char *name, int container_id)
{
    if (!g_initialized) {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    /* Destroy container if ID is valid */
    if (container_id >= 0 && container_id < CONFIG_MAX_CONTAINERS) {
        /* Stop first if running */
        akira_container_status_t status = akira_runtime_get_status(container_id);
        if (status == AKIRA_CONTAINER_RUNNING) {
            akira_runtime_stop(container_id);
        }
        akira_runtime_destroy(container_id);
    }

    /* Delete binary */
    if (name) {
        akira_runtime_delete_binary(name);
    }

    LOG_INF("Uninstalled app: %s", name ? name : "(unknown)");
    return 0;
}

/* ===== Status & Query ===== */

akira_container_status_t akira_runtime_get_status(int container_id)
{
    if (!g_initialized) {
        return AKIRA_CONTAINER_UNKNOWN;
    }

    if (container_id < 0 || container_id >= CONFIG_MAX_CONTAINERS) {
        return AKIRA_CONTAINER_UNKNOWN;
    }

    ocre_container_status_t status = ocre_container_runtime_get_container_status(
        &g_ctx, container_id);

    /* Map OCRE status to Akira status */
    switch (status) {
    case CONTAINER_STATUS_UNKNOWN:
        return AKIRA_CONTAINER_UNKNOWN;
    case CONTAINER_STATUS_CREATED:
        return AKIRA_CONTAINER_CREATED;
    case CONTAINER_STATUS_RUNNING:
        return AKIRA_CONTAINER_RUNNING;
    case CONTAINER_STATUS_STOPPED:
        return AKIRA_CONTAINER_STOPPED;
    case CONTAINER_STATUS_DESTROYED:
        return AKIRA_CONTAINER_DESTROYED;
    case CONTAINER_STATUS_ERROR:
    default:
        return AKIRA_CONTAINER_ERROR;
    }
}

int akira_runtime_list(akira_container_info_t *out_list, int max_count)
{
    if (!g_initialized || !out_list || max_count <= 0) {
        return -EINVAL;
    }

    int count = 0;
    for (int i = 0; i < CONFIG_MAX_CONTAINERS && count < max_count; i++) {
        ocre_container_status_t status = g_ctx.containers[i].container_runtime_status;
        
        /* Skip unused slots */
        if (status == CONTAINER_STATUS_UNKNOWN || status == CONTAINER_STATUS_DESTROYED) {
            continue;
        }

        out_list[count].id = i;
        strncpy(out_list[count].name, 
                g_ctx.containers[i].ocre_container_data.name,
                sizeof(out_list[count].name) - 1);
        out_list[count].name[sizeof(out_list[count].name) - 1] = '\0';
        
        /* Map status */
        switch (status) {
        case CONTAINER_STATUS_CREATED:
            out_list[count].status = AKIRA_CONTAINER_CREATED;
            break;
        case CONTAINER_STATUS_RUNNING:
            out_list[count].status = AKIRA_CONTAINER_RUNNING;
            break;
        case CONTAINER_STATUS_STOPPED:
            out_list[count].status = AKIRA_CONTAINER_STOPPED;
            break;
        default:
            out_list[count].status = AKIRA_CONTAINER_ERROR;
            break;
        }
        
        count++;
    }

    LOG_DBG("Listed %d containers", count);
    return count;
}
