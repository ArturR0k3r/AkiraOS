/**
 * @file akira_runtime.c
 * @brief AkiraOS Runtime Implementation
 *
 * Thin wrapper around OCRE container runtime (new simplified API).
 * Uses the new OCRE Context/Container API.
 */

#include "akira_runtime.h"
#include "../storage/fs_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <errno.h>

/* Include OCRE headers after Zephyr headers to avoid conflicts */
#include <ocre/library.h>
#include <ocre/context.h>
#include <ocre/container.h>

LOG_MODULE_REGISTER(akira_runtime, CONFIG_AKIRA_LOG_LEVEL);

/* ===== Configuration ===== */

/* OCRE's image directory */
#define OCRE_IMAGE_PATH "/lfs/ocre/images"
#define OCRE_WORKDIR "/lfs/ocre"
#define MAX_PATH_LEN 64
#define MAX_CONTAINERS 8

/* ===== Container Registry ===== */

typedef struct {
    int id;
    struct ocre_container *container;
    char name[32];
    bool in_use;
} container_entry_t;

/* ===== Static State ===== */

static struct ocre_context *g_ctx = NULL;
static bool g_initialized = false;
static container_entry_t g_containers[MAX_CONTAINERS] = {0};
static int g_container_counter = 0;
static K_MUTEX_DEFINE(g_container_lock);

/* ===== Initialization ===== */

int akira_runtime_init(void)
{
    if (g_initialized)
    {
        LOG_WRN("Akira runtime already initialized");
        return 0;
    }

    LOG_INF("Initializing Akira runtime...");

    /* Initialize OCRE library - this registers the WAMR runtime */
    int ret = ocre_initialize(NULL);
    if (ret != 0)
    {
        LOG_ERR("Failed to initialize OCRE library: %d", ret);
        return -EIO;
    }

    /* Create OCRE context - this is the container manager */
    g_ctx = ocre_create_context(OCRE_WORKDIR);
    if (g_ctx == NULL)
    {
        LOG_WRN("Failed to create OCRE context at %s (filesystem may be unavailable)", OCRE_WORKDIR);
        LOG_INF("Falling back to RAM-only operation");
        /* Don't fail - app manager will use RAM storage as fallback */
    }

    /* Register Akira native exports with OCRE so WASM apps can call into
     * Akira display/input APIs without modifying the OCRE core.
     */
    extern int register_akira_native_module(void);
    if (register_akira_native_module() != 0)
    {
        LOG_WRN("Akira: failed to register native module with OCRE");
    }

    g_initialized = true;
    LOG_INF("Akira runtime initialized successfully");
    return 0;
}

bool akira_runtime_is_initialized(void)
{
    return g_initialized;
}

/* ===== Container Registry Helpers ===== */

/**
 * @brief Get container pointer from registry by ID
 */
static struct ocre_container *get_container(int id)
{
    if (id <= 0 || id > g_container_counter)
        return NULL;
    
    container_entry_t *e = &g_containers[id % MAX_CONTAINERS];
    if (e->id == id && e->in_use)
        return e->container;
    
    return NULL;
}

/**
 * @brief Register container in registry and return ID
 */
static int register_container(struct ocre_container *container, const char *name)
{
    if (!container || !name)
        return -EINVAL;
    
    k_mutex_lock(&g_container_lock, K_FOREVER);
    
    int cid = ++g_container_counter;
    container_entry_t *e = &g_containers[cid % MAX_CONTAINERS];
    
    e->id = cid;
    e->container = container;
    e->in_use = true;
    strncpy(e->name, name, sizeof(e->name) - 1);
    
    k_mutex_unlock(&g_container_lock);
    
    LOG_DBG("Container %d registered: %s", cid, name);
    return cid;
}

/**
 * @brief Unregister container from registry
 */
static void unregister_container(int id)
{
    if (id <= 0 || id > g_container_counter)
        return;
    
    k_mutex_lock(&g_container_lock, K_FOREVER);
    
    container_entry_t *e = &g_containers[id % MAX_CONTAINERS];
    if (e->id == id && e->in_use) {
        e->in_use = false;
        e->container = NULL;
        LOG_DBG("Container %d unregistered", id);
    }
    
    k_mutex_unlock(&g_container_lock);
}



int akira_runtime_save_binary(const char *name, const void *binary, size_t size)
{
    if (!name || !binary || size == 0)
    {
        return -EINVAL;
    }

    /* Construct path: /lfs/ocre/images/{name}.bin */
    char path[MAX_PATH_LEN];
    int ret = snprintf(path, sizeof(path), "%s/%s.bin", OCRE_IMAGE_PATH, name);
    if (ret < 0 || ret >= sizeof(path))
    {
        LOG_ERR("Path too long for app: %s", name);
        return -ENAMETOOLONG;
    }

    /* Ensure directory exists */
    struct fs_dirent dirent;
    if (fs_stat(OCRE_IMAGE_PATH, &dirent) != 0)
    {
        ret = fs_mkdir(OCRE_IMAGE_PATH);
        if (ret != 0 && ret != -EEXIST)
        {
            LOG_ERR("Failed to create OCRE images directory: %d", ret);
            return ret;
        }
    }

    /* Write binary to file */
    struct fs_file_t file;
    fs_file_t_init(&file);

    ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret != 0)
    {
        LOG_ERR("Failed to create file %s: %d", path, ret);
        return ret;
    }

    ssize_t written = fs_write(&file, binary, size);
    fs_close(&file);

    if (written != (ssize_t)size)
    {
        LOG_ERR("Failed to write full binary to %s (wrote %d of %zu)", path, written, size);
        fs_unlink(path);
        return -EIO;
    }

    LOG_INF("Binary saved: %s (%zu bytes)", path, size);
    return 0;
}

int akira_runtime_delete_binary(const char *name)
{
    if (!name)
    {
        return -EINVAL;
    }

    char path[MAX_PATH_LEN];
    int ret = snprintf(path, sizeof(path), "%s/%s.bin", OCRE_IMAGE_PATH, name);
    if (ret < 0 || ret >= sizeof(path))
    {
        return -ENAMETOOLONG;
    }

    ret = fs_unlink(path);
    if (ret == 0 || ret == -ENOENT)
    {
        LOG_INF("Binary deleted or not found: %s", path);
        return 0;
    }

    LOG_ERR("Failed to delete binary %s: %d", path, ret);
    return ret;
}

/* ===== Container Management ===== */

int akira_runtime_install(const char *name, const void *binary, size_t size)
{
    if (!g_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    if (!name || !binary || size == 0)
    {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    /* Save binary to filesystem */
    int ret = akira_runtime_save_binary(name, binary, size);
    if (ret < 0)
    {
        LOG_ERR("Failed to save binary for %s: %d", name, ret);
        return ret;
    }

    /* Create container from the image
     * The image path is relative to the context's working directory
     * So we use just the filename with .bin extension
     */
    char image_filename[MAX_PATH_LEN];
    ret = snprintf(image_filename, sizeof(image_filename), "%s.bin", name);
    if (ret < 0 || ret >= sizeof(image_filename))
    {
        LOG_ERR("Image filename too long");
        akira_runtime_delete_binary(name);
        return -ENAMETOOLONG;
    }

    /* Create container with auto-detected runtime (NULL)
     * The new OCRE will automatically select "wamr/wasip1" for WASM binaries
     */
    struct ocre_container *container = ocre_context_create_container(
        g_ctx,
        image_filename,    /* image path */
        NULL,              /* runtime - auto-detect as wamr/wasip1 */
        name,              /* container_id */
        false,             /* detached - don't run automatically */
        NULL               /* arguments - none for now */
    );

    if (container == NULL)
    {
        LOG_ERR("Failed to create container for %s", name);
        akira_runtime_delete_binary(name);
        return -EIO;
    }

    /* Register container in registry and return ID */
    int container_id = register_container(container, name);
    
    LOG_INF("Container installed: %s (ID=%d)", name, container_id);
    return container_id;
}

int akira_runtime_start(int container_id)
{
    if (!g_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    struct ocre_container *container = get_container(container_id);
    if (!container)
    {
        LOG_ERR("Container %d not found", container_id);
        return -ENOENT;
    }

    int ret = ocre_container_start(container);
    if (ret == 0) {
        LOG_INF("Container %d started successfully", container_id);
    } else {
        LOG_ERR("Failed to start container %d: %d", container_id, ret);
    }
    
    return ret;
}

int akira_runtime_stop(int container_id)
{
    if (!g_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    struct ocre_container *container = get_container(container_id);
    if (!container)
    {
        LOG_ERR("Container %d not found", container_id);
        return -ENOENT;
    }

    int ret = ocre_container_stop(container);
    if (ret == 0) {
        LOG_INF("Container %d stopped successfully", container_id);
    } else {
        LOG_ERR("Failed to stop container %d: %d", container_id, ret);
    }
    
    return ret;
}

int akira_runtime_uninstall(const char *name, int container_id)
{
    if (!g_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    if (!name)
    {
        LOG_ERR("Invalid app name");
        return -EINVAL;
    }

    /* Stop container if ID is valid */
    if (container_id > 0) {
        struct ocre_container *container = get_container(container_id);
        if (container) {
            ocre_container_kill(container);
            unregister_container(container_id);
            LOG_DBG("Container %d destroyed", container_id);
        }
    }

    /* Delete binary */
    int ret = akira_runtime_delete_binary(name);
    
    LOG_INF("App uninstalled: %s", name);
    return ret;
}

int akira_runtime_get_app_count(void)
{
    if (!g_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    return ocre_context_get_container_count(g_ctx);
}

int akira_runtime_get_app_status(int container_id)
{
    if (!g_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    struct ocre_container *container = get_container(container_id);
    if (!container)
    {
        LOG_ERR("Container %d not found", container_id);
        return -ENOENT;
    }

    ocre_container_status_t status = ocre_container_get_status(container);
    return (int)status;
}

int akira_runtime_dump_status(void)
{
    if (!g_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    int container_count = ocre_context_get_container_count(g_ctx);
    LOG_INF("=== Akira Runtime Status ===");
    LOG_INF("Total containers: %d", container_count);

    if (container_count > 0)
    {
        struct ocre_container *containers[32];
        int listed = ocre_context_get_containers(g_ctx, containers, 32);

        for (int i = 0; i < listed; i++)
        {
            if (containers[i])
            {
                const char *id = ocre_container_get_id(containers[i]);
                const char *image = ocre_container_get_image(containers[i]);
                ocre_container_status_t status = ocre_container_get_status(containers[i]);

                LOG_INF("  [%d] ID=%s Image=%s Status=%d", i, id, image, status);
            }
        }
    }

    return 0;
}

int akira_runtime_destroy(int container_id)
{
    if (!g_initialized)
    {
        LOG_ERR("Runtime not initialized");
        return -ENODEV;
    }

    struct ocre_container *container = get_container(container_id);
    if (!container)
    {
        LOG_ERR("Container %d not found", container_id);
        return -ENOENT;
    }

    /* Stop if running */
    ocre_container_status_t status = ocre_container_get_status(container);
    if (status == OCRE_CONTAINER_STATUS_RUNNING || 
        status == OCRE_CONTAINER_STATUS_PAUSED) {
        ocre_container_kill(container);
        LOG_DBG("Killed running container %d", container_id);
    }

    /* Unregister from our registry */
    unregister_container(container_id);
    
    LOG_INF("Container %d destroyed", container_id);
    return 0;
}
