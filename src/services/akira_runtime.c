/**
 * @file akira_runtime.c
 * @brief AkiraOS Runtime Implementation
 *
 * WASM runtime wrapper using WASM Micro Runtime (WAMR).
 * Provides WASM instance management and execution.
 */

#include "akira_runtime.h"
#include "../storage/fs_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <errno.h>

#ifdef CONFIG_WAMR_ENABLE
#include "wasm_c_api.h"
#include "wasm_export.h"
#endif

LOG_MODULE_REGISTER(akira_runtime, CONFIG_AKIRA_LOG_LEVEL);

/* ===== Configuration ===== */

/* WASM app storage path */
#define WASM_APPS_PATH "/lfs/wasm/apps"
#define MAX_PATH_LEN 64
#define MAX_WASM_INSTANCES 8

/* ===== Static State ===== */

#ifdef CONFIG_WAMR_ENABLE
static wasm_engine_t g_engine = NULL;
static wasm_module_t g_modules[MAX_WASM_INSTANCES] = {0};
static wasm_instance_t g_instances[MAX_WASM_INSTANCES] = {0};
#endif

static bool g_initialized = false;

/* ===== Initialization ===== */

int akira_runtime_init(void)
{
    if (g_initialized)
    {
        LOG_WRN("Akira runtime already initialized");
        return 0;
    }

    LOG_INF("Initializing Akira runtime...");

#ifdef CONFIG_WAMR_ENABLE
    /* Initialize WAMR runtime */
    g_engine = wasm_engine_new();
    if (!g_engine)
    {
        LOG_ERR("Failed to create WAMR engine");
        return -ENOMEM;
    }
    LOG_INF("WAMR engine initialized");
#else
    LOG_WRN("WAMR not enabled - WASM support disabled");
#endif

    /* Ensure storage directory exists */
    struct fs_dirent entry;
    if (fs_stat(WASM_APPS_PATH, &entry) != 0)
    {
        LOG_INF("Creating WASM apps directory: %s", WASM_APPS_PATH);
        fs_mkdir("/lfs");
        fs_mkdir("/lfs/wasm");
        fs_mkdir(WASM_APPS_PATH);
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
    if (!name || !binary || size == 0)
    {
        return -EINVAL;
    }

    /* Construct path: /lfs/wasm/apps/{name}.wasm */
    char path[MAX_PATH_LEN];
    int ret = snprintf(path, sizeof(path), "%s/%s.wasm", WASM_APPS_PATH, name);
    if (ret < 0 || ret >= sizeof(path))
    {
        LOG_ERR("Path too long for app: %s", name);
        return -ENAMETOOLONG;
    }

    /* Ensure directory exists */
    struct fs_dirent entry;
    if (fs_stat(WASM_APPS_PATH, &entry) != 0)
    {
        LOG_INF("Creating WASM apps directory: %s", WASM_APPS_PATH);
        fs_mkdir("/lfs");
        fs_mkdir("/lfs/wasm");
        fs_mkdir(WASM_APPS_PATH);
    }

    /* Try filesystem first, fall back to RAM storage via fs_manager */
    struct fs_file_t file;
    fs_file_t_init(&file);

    ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
    if (ret == 0)
    {
        /* Write to real filesystem */
        ssize_t written = fs_write(&file, binary, size);
        fs_close(&file);

        if (written == size)
        {
            LOG_INF("Saved WASM binary to filesystem: %s (%zu bytes)", path, size);
            return 0;
        }
        LOG_WRN("Filesystem write incomplete: %zd/%zu", written, size);
        fs_unlink(path); /* Clean up partial write */
    }
    else
    {
        LOG_WRN("Filesystem not available (err %d), using RAM fallback", ret);
    }

    /* Fallback to fs_manager (RAM storage) */
    ret = fs_manager_write_file(path, binary, size);
    if (ret < 0)
    {
        LOG_ERR("Failed to save binary: %d", ret);
        return ret;
    }

    LOG_INF("Saved WASM binary to RAM storage: %s (%zu bytes)", path, size);
    return 0;
}

int akira_runtime_delete_binary(const char *name)
{
    if (!name)
    {
        return -EINVAL;
    }

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s.wasm", WASM_APPS_PATH, name);

    /* Try filesystem first */
    int ret = fs_unlink(path);
    if (ret == 0)
    {
        LOG_INF("Deleted WASM binary from filesystem: %s", path);
        return 0;
    }

    /* Try fs_manager (RAM storage) */
    ret = fs_manager_delete_file(path);
    if (ret == 0)
    {
        LOG_INF("Deleted WASM binary from RAM storage: %s", path);
        return 0;
    }

    LOG_WRN("WASM binary not found or delete failed: %s", path);
    return ret;
}

/* ===== WASM Instance Operations ===== */

#ifdef CONFIG_WAMR_ENABLE

int akira_runtime_install(const char *name, const void *binary, size_t size)
{
    if (!g_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    if (!name || !binary || size == 0)
    {
        return -EINVAL;
    }

    /* Save binary to storage */
    int ret = akira_runtime_save_binary(name, binary, size);
    if (ret < 0)
    {
        LOG_ERR("Failed to save WASM binary for %s: %d", name, ret);
        return ret;
    }

    /* Find available slot for module storage */
    int slot = -1;
    for (int i = 0; i < MAX_WASM_INSTANCES; i++)
    {
        if (g_modules[i] == NULL)
        {
            slot = i;
            break;
        }
    }

    if (slot < 0)
    {
        LOG_ERR("No available slots for WASM module");
        akira_runtime_delete_binary(name);
        return -ENOMEM;
    }

    /* Load module from binary */
    char error_buf[128];
    wasm_module_t module = wasm_runtime_load((uint8_t *)binary, size, error_buf, sizeof(error_buf));
    if (!module)
    {
        LOG_ERR("Failed to load WASM module: %s", error_buf);
        akira_runtime_delete_binary(name);
        return -EINVAL;
    }

    g_modules[slot] = module;
    LOG_INF("WASM module loaded: %s (slot: %d)", name, slot);
    return slot;
}

int akira_runtime_start(int instance_id)
{
    if (!g_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    if (instance_id < 0 || instance_id >= MAX_WASM_INSTANCES)
    {
        LOG_ERR("Invalid instance ID: %d", instance_id);
        return -EINVAL;
    }

    if (g_modules[instance_id] == NULL)
    {
        LOG_ERR("Module not loaded in slot %d", instance_id);
        return -ENOENT;
    }

    if (g_instances[instance_id] != NULL)
    {
        LOG_WRN("Instance already running in slot %d", instance_id);
        return 0;
    }

    /* Create instance from module */
    char error_buf[128];
    g_instances[instance_id] = wasm_runtime_instantiate(
        g_modules[instance_id], 256 * 1024, error_buf, sizeof(error_buf));

    if (!g_instances[instance_id])
    {
        LOG_ERR("Failed to instantiate WASM module: %s", error_buf);
        return -ENOMEM;
    }

    LOG_INF("WASM instance created for slot %d", instance_id);
    return 0;
}

int akira_runtime_stop(int instance_id)
{
    if (!g_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    if (instance_id < 0 || instance_id >= MAX_WASM_INSTANCES)
    {
        LOG_ERR("Invalid instance ID: %d", instance_id);
        return -EINVAL;
    }

    if (g_instances[instance_id] != NULL)
    {
        wasm_runtime_deinstantiate(g_instances[instance_id]);
        g_instances[instance_id] = NULL;
        LOG_INF("WASM instance stopped (slot %d)", instance_id);
    }

    return 0;
}

int akira_runtime_destroy(int instance_id)
{
    if (!g_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    if (instance_id < 0 || instance_id >= MAX_WASM_INSTANCES)
    {
        LOG_ERR("Invalid instance ID: %d", instance_id);
        return -EINVAL;
    }

    /* Stop instance if running */
    if (g_instances[instance_id] != NULL)
    {
        wasm_runtime_deinstantiate(g_instances[instance_id]);
        g_instances[instance_id] = NULL;
    }

    /* Unload module */
    if (g_modules[instance_id] != NULL)
    {
        wasm_runtime_unload(g_modules[instance_id]);
        g_modules[instance_id] = NULL;
    }

    LOG_INF("WASM module destroyed (slot %d)", instance_id);
    return 0;
}

int akira_runtime_uninstall(const char *name, int instance_id)
{
    if (!g_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    /* Destroy instance if ID is valid */
    if (instance_id >= 0 && instance_id < MAX_WASM_INSTANCES)
    {
        akira_runtime_stop(instance_id);
        akira_runtime_destroy(instance_id);
    }

    /* Delete binary */
    if (name)
    {
        akira_runtime_delete_binary(name);
    }

    LOG_INF("Uninstalled WASM app: %s", name ? name : "(unknown)");
    return 0;
}

akira_container_status_t akira_runtime_get_status(int instance_id)
{
    if (!g_initialized)
    {
        return AKIRA_CONTAINER_UNKNOWN;
    }

    if (instance_id < 0 || instance_id >= MAX_WASM_INSTANCES)
    {
        return AKIRA_CONTAINER_UNKNOWN;
    }

    if (g_instances[instance_id] == NULL)
    {
        if (g_modules[instance_id] == NULL)
            return AKIRA_CONTAINER_UNKNOWN;
        return AKIRA_CONTAINER_STOPPED;
    }

    /* Instance exists and is running */
    return AKIRA_CONTAINER_RUNNING;
}

int akira_runtime_list(akira_container_info_t *out_list, int max_count)
{
    if (!g_initialized || !out_list || max_count <= 0)
    {
        return -EINVAL;
    }

    int count = 0;
    for (int i = 0; i < MAX_WASM_INSTANCES && count < max_count; i++)
    {
        if (g_modules[i] == NULL)
            continue;

        out_list[count].id = i;
        snprintf(out_list[count].name, sizeof(out_list[count].name), "wasm_app_%d", i);

        if (g_instances[i] != NULL)
        {
            out_list[count].status = AKIRA_CONTAINER_RUNNING;
        }
        else
        {
            out_list[count].status = AKIRA_CONTAINER_STOPPED;
        }

        count++;
    }

    LOG_DBG("Listed %d WASM instances", count);
    return count;
}

#else /* CONFIG_WAMR_ENABLE not defined - provide stubs */

int akira_runtime_install(const char *name, const void *binary, size_t size)
{
    LOG_ERR("WAMR not enabled - cannot install WASM apps");
    return -ENOTSUP;
}

int akira_runtime_start(int instance_id)
{
    LOG_ERR("WAMR not enabled");
    return -ENOTSUP;
}

int akira_runtime_stop(int instance_id)
{
    return -ENOTSUP;
}

int akira_runtime_destroy(int instance_id)
{
    return -ENOTSUP;
}

int akira_runtime_uninstall(const char *name, int instance_id)
{
    return -ENOTSUP;
}

akira_container_status_t akira_runtime_get_status(int instance_id)
{
    return AKIRA_CONTAINER_UNKNOWN;
}

int akira_runtime_list(akira_container_info_t *out_list, int max_count)
{
    return 0;
}

#endif /* CONFIG_WAMR_ENABLE */
