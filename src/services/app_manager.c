/**
 * @file app_manager.c
 * @brief AkiraOS App Manager Implementation
 *
 * Core app management: registry, lifecycle, installation, crash handling.
 */

#include "app_manager.h"
#include "akira_runtime.h"
#include "../storage/fs_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/crc.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(app_manager, CONFIG_AKIRA_LOG_LEVEL);

/* ===== Configuration ===== */

#define REGISTRY_PATH "/lfs/apps/registry.bin"
#define APPS_DIR "/lfs/apps"
#define APP_DATA_DIR "/lfs/app_data"
#define REGISTRY_MAGIC 0x414B4150 /* "AKAP" */
#define REGISTRY_VERSION 1
#define MAX_WASM_MAGIC 8

/* WASM magic bytes: \0asm */
static const uint8_t WASM_MAGIC[] = {0x00, 0x61, 0x73, 0x6D};

/* ===== Internal Types ===== */

typedef struct
{
    uint32_t magic;
    uint8_t version;
    uint8_t app_count;
    uint16_t reserved;
    uint32_t crc;
} registry_header_t;

typedef struct
{
    char name[APP_NAME_MAX_LEN];
    size_t total_size;
    size_t received;
    app_source_t source;
    uint8_t *buffer;
    bool active;
} install_session_t;

/* ===== Static State ===== */

static app_entry_t g_registry[CONFIG_AKIRA_APP_MAX_INSTALLED];
static uint8_t g_app_count = 0;
static bool g_initialized = false;
static K_MUTEX_DEFINE(g_registry_mutex);

/* Install sessions for chunked upload */
#define MAX_INSTALL_SESSIONS 2
static install_session_t g_sessions[MAX_INSTALL_SESSIONS];

/* State change callback */
static app_state_change_cb_t g_state_cb = NULL;
static void *g_state_cb_user = NULL;

/* Auto-restart work */
static struct k_work_delayable g_restart_work;
static char g_restart_app_name[APP_NAME_MAX_LEN];

/* ===== Forward Declarations ===== */

static int registry_load(void);
static int registry_save(void);
static app_entry_t *find_app_by_name(const char *name);
static app_entry_t *find_free_slot(void);
static int validate_wasm(const void *binary, size_t size);
static int save_app_binary(const char *name, const void *binary, size_t size);
static int delete_app_binary(const char *name);
static void set_app_state(app_entry_t *app, app_state_t new_state);
static void restart_work_handler(struct k_work *work);
static int ensure_dirs_exist(void);

/* ===== Initialization ===== */

int app_manager_init(void)
{
    if (g_initialized)
    {
        LOG_WRN("App Manager already initialized");
        return 0;
    }

    LOG_INF("Initializing App Manager");

    /* Initialize Akira runtime (OCRE + storage) */
    int ret = akira_runtime_init();
    if (ret < 0)
    {
        LOG_ERR("Failed to initialize Akira runtime: %d", ret);
        return ret;
    }

    /* Ensure directories exist */
    ret = ensure_dirs_exist();
    if (ret < 0)
    {
        LOG_WRN("Failed to create app directories: %d", ret);
        /* Continue anyway - might be read-only */
    }

    /* Initialize registry */
    memset(g_registry, 0, sizeof(g_registry));
    memset(g_sessions, 0, sizeof(g_sessions));
    g_app_count = 0;

    /* Load registry from flash */
    ret = registry_load();
    if (ret < 0)
    {
        LOG_WRN("No registry found or load failed, starting fresh");
    }
    else
    {
        LOG_INF("Loaded %d apps from registry", g_app_count);
    }

    /* Initialize restart work */
    k_work_init_delayable(&g_restart_work, restart_work_handler);

    g_initialized = true;
    LOG_INF("App Manager initialized, %d/%d slots used",
            g_app_count, CONFIG_AKIRA_APP_MAX_INSTALLED);

    return 0;
}

void app_manager_shutdown(void)
{
    if (!g_initialized)
    {
        return;
    }

    LOG_INF("Shutting down App Manager");

    /* Stop all running apps */
    k_mutex_lock(&g_registry_mutex, K_FOREVER);

    for (int i = 0; i < CONFIG_AKIRA_APP_MAX_INSTALLED; i++)
    {
        if (g_registry[i].state == APP_STATE_RUNNING && g_registry[i].container_id >= 0)
        {
            LOG_INF("Stopping app: %s (container %d)", g_registry[i].name, g_registry[i].container_id);
            akira_runtime_stop(g_registry[i].container_id);
            g_registry[i].state = APP_STATE_STOPPED;
        }
    }

    /* Save registry */
    registry_save();

    k_mutex_unlock(&g_registry_mutex);

    g_initialized = false;
    LOG_INF("App Manager shutdown complete");
}

/* ===== Installation ===== */

int app_manager_install(const char *name, const void *binary, size_t size,
                        const app_manifest_t *manifest, app_source_t source)
{
    if (!g_initialized)
    {
        return -ENODEV;
    }

    if (!binary || size == 0)
    {
        return -EINVAL;
    }

    /* Validate WASM binary */
    int ret = validate_wasm(binary, size);
    if (ret < 0)
    {
        LOG_ERR("Invalid WASM binary");
        return ret;
    }

    /* Check size limit */
    if (size > CONFIG_AKIRA_APP_MAX_SIZE_KB * 1024)
    {
        LOG_ERR("App too large: %zu > %dKB", size, CONFIG_AKIRA_APP_MAX_SIZE_KB);
        return -EFBIG;
    }

    k_mutex_lock(&g_registry_mutex, K_FOREVER);

    /* Determine app name */
    char app_name[APP_NAME_MAX_LEN];
    if (name && name[0])
    {
        strncpy(app_name, name, APP_NAME_MAX_LEN - 1);
        app_name[APP_NAME_MAX_LEN - 1] = '\0';
    }
    else if (manifest && manifest->name[0])
    {
        strncpy(app_name, manifest->name, APP_NAME_MAX_LEN - 1);
        app_name[APP_NAME_MAX_LEN - 1] = '\0';
    }
    else
    {
        snprintf(app_name, APP_NAME_MAX_LEN, "app_%08x",
                 crc32_ieee((const uint8_t *)binary, size > 256 ? 256 : size));
    }

    /* Check if already exists */
    app_entry_t *existing = find_app_by_name(app_name);
    if (existing)
    {
        /* Update existing app */
        LOG_INF("Updating existing app: %s", app_name);

        /* Stop if running */
        if (existing->state == APP_STATE_RUNNING && existing->container_id >= 0)
        {
            akira_runtime_stop(existing->container_id);
        }

        /* Destroy old container */
        if (existing->container_id >= 0)
        {
            akira_runtime_destroy(existing->container_id);
            existing->container_id = -1;
        }
    }
    else
    {
        /* Find free slot */
        existing = find_free_slot();
        if (!existing)
        {
            LOG_ERR("No free slots, max %d apps", CONFIG_AKIRA_APP_MAX_INSTALLED);
            k_mutex_unlock(&g_registry_mutex);
            return -ENOMEM;
        }

        /* Assign ID */
        existing->id = (uint8_t)(existing - g_registry) + 1;
        g_app_count++;
    }

    /* Populate name BEFORE saving binary (so save_app_binary can find the entry) */
    strncpy(existing->name, app_name, APP_NAME_MAX_LEN);

    /* Save binary to flash */
    ret = save_app_binary(app_name, binary, size);
    if (ret < 0)
    {
        LOG_ERR("Failed to save app binary: %d", ret);
        if (existing->name[0] == '\0')
        {
            g_app_count--;
        }
        existing->name[0] = '\0';  /* Clear name on failure */
        k_mutex_unlock(&g_registry_mutex);
        return ret;
    }

    /* Populate rest of entry */
    existing->source = source;
    existing->size = size;
    existing->container_id = -1;
    existing->crash_count = 0;
    existing->install_time = k_uptime_get_32() / 1000;
    existing->is_preloaded = (source == APP_SOURCE_FIRMWARE);

    /* Apply manifest or defaults */
    if (manifest)
    {
        strncpy(existing->version, manifest->version, APP_VERSION_MAX_LEN);
        existing->heap_kb = manifest->heap_kb;
        existing->stack_kb = manifest->stack_kb;
        existing->permissions = manifest->permissions;
        existing->restart = manifest->restart;
    }
    else
    {
        strncpy(existing->version, "0.0.0", APP_VERSION_MAX_LEN);
        existing->heap_kb = CONFIG_AKIRA_APP_DEFAULT_HEAP_KB;
        existing->stack_kb = CONFIG_AKIRA_APP_DEFAULT_STACK_KB;
        existing->permissions = APP_PERM_NONE;
        existing->restart.enabled = false;
        existing->restart.max_retries = CONFIG_AKIRA_APP_MAX_RETRIES;
        existing->restart.delay_ms = CONFIG_AKIRA_APP_RESTART_DELAY_MS;
    }

    set_app_state(existing, APP_STATE_INSTALLED);

    /* Save registry */
    registry_save();

    k_mutex_unlock(&g_registry_mutex);

    LOG_INF("Installed app: %s (ID: %d, size: %zu)", app_name, existing->id, size);
    return existing->id;
}

int app_manager_install_from_path(const char *path)
{
    if (!path)
    {
        return -EINVAL;
    }

    /* Get file size using fs_manager */
    ssize_t size = fs_manager_get_size(path);
    if (size < 0)
    {
        LOG_ERR("Failed to get size of %s: %zd", path, size);
        return (int)size;
    }

    if (size > CONFIG_AKIRA_APP_MAX_SIZE_KB * 1024)
    {
        LOG_ERR("App too large: %zd bytes", size);
        return -EFBIG;
    }

    /* Allocate buffer */
    uint8_t *buffer = k_malloc(size);
    if (!buffer)
    {
        LOG_ERR("Failed to allocate %zd bytes", size);
        return -ENOMEM;
    }

    /* Read file using fs_manager */
    ssize_t bytes_read = fs_manager_read_file(path, buffer, size);
    if (bytes_read != size)
    {
        LOG_ERR("Read failed: %zd != %zd", bytes_read, size);
        k_free(buffer);
        return -EIO;
    }

    /* Extract name from path */
    const char *filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;

    char name[APP_NAME_MAX_LEN];
    strncpy(name, filename, APP_NAME_MAX_LEN - 1);
    name[APP_NAME_MAX_LEN - 1] = '\0';

    /* Remove .wasm extension */
    char *ext = strstr(name, ".wasm");
    if (ext)
    {
        *ext = '\0';
    }

    /* Determine source from path */
    app_source_t source = APP_SOURCE_UNKNOWN;
    if (strstr(path, "/sd/"))
    {
        source = APP_SOURCE_SD;
    }
    else if (strstr(path, "/usb/"))
    {
        source = APP_SOURCE_USB;
    }

    /* Try to load manifest */
    app_manifest_t manifest;
    char manifest_path[APP_PATH_MAX_LEN];
    snprintf(manifest_path, sizeof(manifest_path), "%.*s.json",
             (int)(ext ? ext - name : strlen(name)), path);

    char json[512];
    ssize_t mf_size = fs_manager_read_file(manifest_path, json, sizeof(json) - 1);
    if (mf_size > 0)
    {
        json[mf_size] = '\0';
        if (app_manifest_parse(json, mf_size, &manifest) == 0)
        {
            int ret = app_manager_install(name, buffer, size, &manifest, source);
            k_free(buffer);
            return ret;
        }
    }

    /* Install without manifest */
    int ret = app_manager_install(name, buffer, size, NULL, source);
    k_free(buffer);
    return ret;
}

int app_manager_uninstall(const char *name)
{
    if (!g_initialized || !name)
    {
        return -EINVAL;
    }

    k_mutex_lock(&g_registry_mutex, K_FOREVER);

    app_entry_t *app = find_app_by_name(name);
    if (!app)
    {
        k_mutex_unlock(&g_registry_mutex);
        LOG_WRN("App not found: %s", name);
        return -ENOENT;
    }

    if (app->is_preloaded)
    {
        k_mutex_unlock(&g_registry_mutex);
        LOG_ERR("Cannot uninstall preloaded app: %s", name);
        return -EPERM;
    }

    /* Stop if running */
    if (app->state == APP_STATE_RUNNING && app->container_id >= 0)
    {
        akira_runtime_stop(app->container_id);
    }

    /* Destroy container and delete binary */
    akira_runtime_uninstall(app->name, app->container_id);

    /* Clear entry */
    memset(app, 0, sizeof(app_entry_t));
    g_app_count--;

    /* Save registry */
    registry_save();

    k_mutex_unlock(&g_registry_mutex);

    LOG_INF("Uninstalled app: %s", name);
    return 0;
}

/* ===== Lifecycle ===== */

int app_manager_start(const char *name)
{
    if (!g_initialized || !name)
    {
        return -EINVAL;
    }

    k_mutex_lock(&g_registry_mutex, K_FOREVER);

    app_entry_t *app = find_app_by_name(name);
    if (!app)
    {
        k_mutex_unlock(&g_registry_mutex);
        return -ENOENT;
    }

    if (app->state == APP_STATE_RUNNING)
    {
        k_mutex_unlock(&g_registry_mutex);
        return 0; /* Already running */
    }

    if (app->state == APP_STATE_FAILED)
    {
        /* Reset crash counter on explicit start */
        app->crash_count = 0;
    }

    /* Check concurrent limit */
    int running = app_manager_get_running_count();
    if (running >= CONFIG_AKIRA_APP_MAX_RUNNING)
    {
        k_mutex_unlock(&g_registry_mutex);
        LOG_ERR("Max concurrent apps reached (%d)", CONFIG_AKIRA_APP_MAX_RUNNING);
        return -EBUSY;
    }

    /* Load app binary if not loaded */
    if (app->container_id < 0)
    {
        /* Read binary from storage (flash or RAM fallback) */
        char path[APP_PATH_MAX_LEN];
        snprintf(path, sizeof(path), "%s/%03d_%s.wasm",
                 APPS_DIR, app->id, app->name);

        uint8_t *buffer = k_malloc(app->size);
        if (!buffer)
        {
            k_mutex_unlock(&g_registry_mutex);
            return -ENOMEM;
        }

        ssize_t bytes_read = fs_manager_read_file(path, buffer, app->size);
        if (bytes_read < 0)
        {
            k_free(buffer);
            k_mutex_unlock(&g_registry_mutex);
            LOG_ERR("Failed to read app binary: %s (err %zd)", path, bytes_read);
            return (int)bytes_read;
        }

        if (bytes_read != app->size)
        {
            k_free(buffer);
            k_mutex_unlock(&g_registry_mutex);
            LOG_ERR("App binary size mismatch: expected %zu, got %zd", app->size, bytes_read);
            return -EIO;
        }

        /* Install into Akira runtime (saves binary + creates container) */
        int load_ret = akira_runtime_install(name, buffer, app->size);
        k_free(buffer);

        if (load_ret < 0)
        {
            k_mutex_unlock(&g_registry_mutex);
            LOG_ERR("Failed to install app into Akira runtime: %d", load_ret);
            return load_ret;
        }

        app->container_id = load_ret;
    }

    /* Start the app by container ID */
    int ret = akira_runtime_start(app->container_id);
    if (ret < 0)
    {
        k_mutex_unlock(&g_registry_mutex);
        LOG_ERR("Failed to start app: %d", ret);
        set_app_state(app, APP_STATE_ERROR);
        return ret;
    }

    app->last_start_time = k_uptime_get_32() / 1000;
    set_app_state(app, APP_STATE_RUNNING);
    registry_save();

    k_mutex_unlock(&g_registry_mutex);

    LOG_INF("Started app: %s", name);
    return 0;
}

int app_manager_stop(const char *name)
{
    if (!g_initialized || !name)
    {
        return -EINVAL;
    }

    k_mutex_lock(&g_registry_mutex, K_FOREVER);

    app_entry_t *app = find_app_by_name(name);
    if (!app)
    {
        k_mutex_unlock(&g_registry_mutex);
        return -ENOENT;
    }

    if (app->state != APP_STATE_RUNNING)
    {
        k_mutex_unlock(&g_registry_mutex);
        return 0; /* Not running */
    }

    if (app->container_id < 0)
    {
        k_mutex_unlock(&g_registry_mutex);
        LOG_ERR("App has no container ID");
        return -EINVAL;
    }

    int ret = akira_runtime_stop(app->container_id);
    if (ret < 0)
    {
        k_mutex_unlock(&g_registry_mutex);
        LOG_ERR("Failed to stop app: %d", ret);
        return ret;
    }

    set_app_state(app, APP_STATE_STOPPED);
    registry_save();

    k_mutex_unlock(&g_registry_mutex);

    LOG_INF("Stopped app: %s", name);
    return 0;
}

int app_manager_restart(const char *name)
{
    if (!g_initialized || !name)
    {
        return -EINVAL;
    }

    k_mutex_lock(&g_registry_mutex, K_FOREVER);

    app_entry_t *app = find_app_by_name(name);
    if (!app)
    {
        k_mutex_unlock(&g_registry_mutex);
        return -ENOENT;
    }

    /* Reset crash counter */
    app->crash_count = 0;

    /* Stop if running */
    if (app->state == APP_STATE_RUNNING && app->container_id >= 0)
    {
        akira_runtime_stop(app->container_id);
        set_app_state(app, APP_STATE_STOPPED);
    }

    k_mutex_unlock(&g_registry_mutex);

    /* Start again */
    return app_manager_start(name);
}

/* ===== Query ===== */

int app_manager_list(app_info_t *out_list, int max_count)
{
    if (!g_initialized || !out_list || max_count <= 0)
    {
        return -EINVAL;
    }

    k_mutex_lock(&g_registry_mutex, K_FOREVER);

    int count = 0;
    for (int i = 0; i < CONFIG_AKIRA_APP_MAX_INSTALLED && count < max_count; i++)
    {
        if (g_registry[i].name[0] != '\0')
        {
            out_list[count].id = g_registry[i].id;
            strncpy(out_list[count].name, g_registry[i].name, APP_NAME_MAX_LEN);
            strncpy(out_list[count].version, g_registry[i].version, APP_VERSION_MAX_LEN);
            out_list[count].state = g_registry[i].state;
            out_list[count].size = g_registry[i].size;
            out_list[count].heap_kb = g_registry[i].heap_kb;
            out_list[count].stack_kb = g_registry[i].stack_kb;
            out_list[count].crash_count = g_registry[i].crash_count;
            out_list[count].auto_restart = g_registry[i].restart.enabled;
            count++;
        }
    }

    k_mutex_unlock(&g_registry_mutex);
    return count;
}

int app_manager_get_info(const char *name, app_info_t *out_info)
{
    if (!g_initialized || !name || !out_info)
    {
        return -EINVAL;
    }

    k_mutex_lock(&g_registry_mutex, K_FOREVER);

    app_entry_t *app = find_app_by_name(name);
    if (!app)
    {
        k_mutex_unlock(&g_registry_mutex);
        return -ENOENT;
    }

    out_info->id = app->id;
    strncpy(out_info->name, app->name, APP_NAME_MAX_LEN);
    strncpy(out_info->version, app->version, APP_VERSION_MAX_LEN);
    out_info->state = app->state;
    out_info->size = app->size;
    out_info->heap_kb = app->heap_kb;
    out_info->stack_kb = app->stack_kb;
    out_info->crash_count = app->crash_count;
    out_info->auto_restart = app->restart.enabled;

    k_mutex_unlock(&g_registry_mutex);
    return 0;
}

app_state_t app_manager_get_state(const char *name)
{
    if (!g_initialized || !name)
    {
        return APP_STATE_NEW;
    }

    k_mutex_lock(&g_registry_mutex, K_FOREVER);
    app_entry_t *app = find_app_by_name(name);
    app_state_t state = app ? app->state : APP_STATE_NEW;
    k_mutex_unlock(&g_registry_mutex);

    return state;
}

int app_manager_get_count(void)
{
    return g_initialized ? g_app_count : 0;
}

int app_manager_get_running_count(void)
{
    if (!g_initialized)
    {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < CONFIG_AKIRA_APP_MAX_INSTALLED; i++)
    {
        if (g_registry[i].state == APP_STATE_RUNNING)
        {
            count++;
        }
    }
    return count;
}

/* ===== Storage Scanning ===== */

int app_manager_scan_dir(const char *path, char names[][APP_NAME_MAX_LEN], int max_count)
{
    if (!path || !names || max_count <= 0)
    {
        return -EINVAL;
    }

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);

    int ret = fs_opendir(&dir, path);
    if (ret < 0)
    {
        LOG_ERR("Failed to open directory: %s (%d)", path, ret);
        return ret;
    }

    int count = 0;
    struct fs_dirent entry;

    while (count < max_count && fs_readdir(&dir, &entry) == 0)
    {
        if (entry.name[0] == '\0')
        {
            break; /* End of directory */
        }

        /* Check for .wasm extension */
        size_t len = strlen(entry.name);
        if (len > 5 && strcmp(&entry.name[len - 5], ".wasm") == 0)
        {
            /* Extract name without extension */
            size_t name_len = len - 5;
            if (name_len >= APP_NAME_MAX_LEN)
            {
                name_len = APP_NAME_MAX_LEN - 1;
            }
            strncpy(names[count], entry.name, name_len);
            names[count][name_len] = '\0';
            count++;
        }
    }

    fs_closedir(&dir);
    LOG_INF("Found %d apps in %s", count, path);
    return count;
}

/* ===== Callbacks ===== */

void app_manager_register_state_cb(app_state_change_cb_t callback, void *user_data)
{
    g_state_cb = callback;
    g_state_cb_user = user_data;
}

/* ===== Chunked Install API ===== */

int app_manager_install_begin(const char *name, size_t total_size, app_source_t source)
{
    if (!g_initialized || !name || total_size == 0)
    {
        return -EINVAL;
    }

    if (total_size > CONFIG_AKIRA_APP_MAX_SIZE_KB * 1024)
    {
        return -EFBIG;
    }

    /* Find free session */
    int session = -1;
    for (int i = 0; i < MAX_INSTALL_SESSIONS; i++)
    {
        if (!g_sessions[i].active)
        {
            session = i;
            break;
        }
    }

    if (session < 0)
    {
        LOG_ERR("No free install sessions");
        return -EBUSY;
    }

    /* Allocate buffer */
    g_sessions[session].buffer = k_malloc(total_size);
    if (!g_sessions[session].buffer)
    {
        LOG_ERR("Failed to allocate install buffer: %zu", total_size);
        return -ENOMEM;
    }

    strncpy(g_sessions[session].name, name, APP_NAME_MAX_LEN);
    g_sessions[session].total_size = total_size;
    g_sessions[session].received = 0;
    g_sessions[session].source = source;
    g_sessions[session].active = true;

    LOG_INF("Install session %d started: %s (%zu bytes)", session, name, total_size);
    return session;
}

int app_manager_install_chunk(int session, const void *data, size_t len)
{
    if (session < 0 || session >= MAX_INSTALL_SESSIONS)
    {
        return -EINVAL;
    }

    if (!g_sessions[session].active || !data || len == 0)
    {
        return -EINVAL;
    }

    if (g_sessions[session].received + len > g_sessions[session].total_size)
    {
        LOG_ERR("Chunk overflow: %zu + %zu > %zu",
                g_sessions[session].received, len, g_sessions[session].total_size);
        return -ENOSPC;
    }

    memcpy(g_sessions[session].buffer + g_sessions[session].received, data, len);
    g_sessions[session].received += len;

    return 0;
}

int app_manager_install_end(int session, const app_manifest_t *manifest)
{
    if (session < 0 || session >= MAX_INSTALL_SESSIONS)
    {
        return -EINVAL;
    }

    if (!g_sessions[session].active)
    {
        return -EINVAL;
    }

    if (g_sessions[session].received != g_sessions[session].total_size)
    {
        LOG_ERR("Incomplete transfer: %zu != %zu",
                g_sessions[session].received, g_sessions[session].total_size);
        app_manager_install_abort(session);
        return -EAGAIN;
    }

    /* Install the app */
    int ret = app_manager_install(
        g_sessions[session].name,
        g_sessions[session].buffer,
        g_sessions[session].total_size,
        manifest,
        g_sessions[session].source);

    /* Clean up session */
    k_free(g_sessions[session].buffer);
    g_sessions[session].buffer = NULL;
    g_sessions[session].active = false;

    return ret;
}

void app_manager_install_abort(int session)
{
    if (session < 0 || session >= MAX_INSTALL_SESSIONS)
    {
        return;
    }

    if (g_sessions[session].buffer)
    {
        k_free(g_sessions[session].buffer);
        g_sessions[session].buffer = NULL;
    }
    g_sessions[session].active = false;

    LOG_INF("Install session %d aborted", session);
}

/* ===== Manifest Helpers ===== */

void app_manifest_init_defaults(app_manifest_t *manifest, const char *name)
{
    if (!manifest)
    {
        return;
    }

    memset(manifest, 0, sizeof(app_manifest_t));

    if (name)
    {
        strncpy(manifest->name, name, APP_NAME_MAX_LEN - 1);
    }
    strncpy(manifest->version, "0.0.0", APP_VERSION_MAX_LEN);
    strncpy(manifest->entry, "_start", sizeof(manifest->entry) - 1);
    manifest->heap_kb = CONFIG_AKIRA_APP_DEFAULT_HEAP_KB;
    manifest->stack_kb = CONFIG_AKIRA_APP_DEFAULT_STACK_KB;
    manifest->restart.enabled = false;
    manifest->restart.max_retries = CONFIG_AKIRA_APP_MAX_RETRIES;
    manifest->restart.delay_ms = CONFIG_AKIRA_APP_RESTART_DELAY_MS;
    manifest->permissions = APP_PERM_NONE;
}

int app_manifest_parse(const char *json, size_t json_len, app_manifest_t *out_manifest)
{
    if (!json || !out_manifest)
    {
        return -EINVAL;
    }

    /* Initialize with defaults */
    app_manifest_init_defaults(out_manifest, NULL);

    /* Simple JSON parsing - just extract key fields */
    /* TODO: Use proper JSON parser like cJSON if available */

    /* Extract "name" */
    const char *name_start = strstr(json, "\"name\"");
    if (name_start)
    {
        name_start = strchr(name_start, ':');
        if (name_start)
        {
            name_start = strchr(name_start, '"');
            if (name_start)
            {
                name_start++;
                const char *name_end = strchr(name_start, '"');
                if (name_end && name_end - name_start < APP_NAME_MAX_LEN)
                {
                    strncpy(out_manifest->name, name_start, name_end - name_start);
                    out_manifest->name[name_end - name_start] = '\0';
                }
            }
        }
    }

    /* Extract "version" */
    const char *ver_start = strstr(json, "\"version\"");
    if (ver_start)
    {
        ver_start = strchr(ver_start, ':');
        if (ver_start)
        {
            ver_start = strchr(ver_start, '"');
            if (ver_start)
            {
                ver_start++;
                const char *ver_end = strchr(ver_start, '"');
                if (ver_end && ver_end - ver_start < APP_VERSION_MAX_LEN)
                {
                    strncpy(out_manifest->version, ver_start, ver_end - ver_start);
                    out_manifest->version[ver_end - ver_start] = '\0';
                }
            }
        }
    }

    /* Extract "heap_kb" */
    const char *heap_start = strstr(json, "\"heap_kb\"");
    if (heap_start)
    {
        heap_start = strchr(heap_start, ':');
        if (heap_start)
        {
            out_manifest->heap_kb = (uint16_t)atoi(heap_start + 1);
        }
    }

    /* Extract "stack_kb" */
    const char *stack_start = strstr(json, "\"stack_kb\"");
    if (stack_start)
    {
        stack_start = strchr(stack_start, ':');
        if (stack_start)
        {
            out_manifest->stack_kb = (uint16_t)atoi(stack_start + 1);
        }
    }

    LOG_DBG("Parsed manifest: name=%s, version=%s, heap=%dKB, stack=%dKB",
            out_manifest->name, out_manifest->version,
            out_manifest->heap_kb, out_manifest->stack_kb);

    return 0;
}

/* ===== State Helpers ===== */

const char *app_state_to_str(app_state_t state)
{
    switch (state)
    {
    case APP_STATE_NEW:
        return "NEW";
    case APP_STATE_INSTALLED:
        return "INSTALLED";
    case APP_STATE_RUNNING:
        return "RUNNING";
    case APP_STATE_STOPPED:
        return "STOPPED";
    case APP_STATE_ERROR:
        return "ERROR";
    case APP_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *app_source_to_str(app_source_t source)
{
    switch (source)
    {
    case APP_SOURCE_HTTP:
        return "HTTP";
    case APP_SOURCE_BLE:
        return "BLE";
    case APP_SOURCE_USB:
        return "USB";
    case APP_SOURCE_SD:
        return "SD";
    case APP_SOURCE_FIRMWARE:
        return "FIRMWARE";
    default:
        return "UNKNOWN";
    }
}

/* ===== Internal Functions ===== */

static int ensure_dirs_exist(void)
{
    /* Use fs_manager to create directories - it handles RAM fallback */
    int ret = fs_manager_mkdir(APPS_DIR);
    if (ret < 0 && ret != -EEXIST)
    {
        LOG_WRN("Failed to create %s: %d (using RAM fallback)", APPS_DIR, ret);
        /* Continue anyway - fs_manager will use RAM if no persistent storage */
    }

    ret = fs_manager_mkdir(APP_DATA_DIR);
    if (ret < 0 && ret != -EEXIST)
    {
        LOG_WRN("Failed to create %s: %d (using RAM fallback)", APP_DATA_DIR, ret);
    }

    return 0;
}

static int registry_load(void)
{
    /* Try to read registry using fs_manager (handles RAM fallback) */
    uint8_t buffer[sizeof(registry_header_t) + CONFIG_AKIRA_APP_MAX_INSTALLED * sizeof(app_entry_t)];
    
    ssize_t read = fs_manager_read_file(REGISTRY_PATH, buffer, sizeof(buffer));
    if (read < (ssize_t)sizeof(registry_header_t))
    {
        LOG_DBG("No registry found or too small: %zd", read);
        return -ENOENT;
    }

    /* Parse header */
    registry_header_t *header = (registry_header_t *)buffer;

    /* Validate header */
    if (header->magic != REGISTRY_MAGIC || header->version != REGISTRY_VERSION)
    {
        LOG_WRN("Invalid registry header");
        return -EINVAL;
    }

    /* Read entries */
    int count = header->app_count;
    if (count > CONFIG_AKIRA_APP_MAX_INSTALLED)
    {
        count = CONFIG_AKIRA_APP_MAX_INSTALLED;
    }

    size_t expected_size = sizeof(registry_header_t) + count * sizeof(app_entry_t);
    if (read < (ssize_t)expected_size)
    {
        LOG_WRN("Registry file truncated");
        return -EIO;
    }

    memcpy(g_registry, buffer + sizeof(registry_header_t), count * sizeof(app_entry_t));
    g_app_count = count;

    /* Reset runtime state */
    for (int i = 0; i < count; i++)
    {
        g_registry[i].container_id = -1;
        if (g_registry[i].state == APP_STATE_RUNNING)
        {
            g_registry[i].state = APP_STATE_INSTALLED;
        }
    }

    LOG_INF("Loaded %d apps from registry", count);
    return 0;
}

static int registry_save(void)
{
    /* Build registry buffer */
    uint8_t buffer[sizeof(registry_header_t) + CONFIG_AKIRA_APP_MAX_INSTALLED * sizeof(app_entry_t)];
    size_t offset = 0;

    /* Write header */
    registry_header_t header = {
        .magic = REGISTRY_MAGIC,
        .version = REGISTRY_VERSION,
        .app_count = g_app_count,
        .reserved = 0,
        .crc = 0, /* TODO: Calculate CRC */
    };

    memcpy(buffer, &header, sizeof(header));
    offset += sizeof(header);

    /* Write entries (only valid ones) */
    for (int i = 0; i < CONFIG_AKIRA_APP_MAX_INSTALLED; i++)
    {
        if (g_registry[i].name[0] != '\0')
        {
            memcpy(buffer + offset, &g_registry[i], sizeof(app_entry_t));
            offset += sizeof(app_entry_t);
        }
    }

    /* Save using fs_manager (handles RAM fallback) */
    ssize_t written = fs_manager_write_file(REGISTRY_PATH, buffer, offset);
    if (written < 0)
    {
        LOG_ERR("Failed to save registry: %zd", written);
        return written;
    }

    LOG_DBG("Saved registry (%zu bytes)", offset);
    return 0;
}

static app_entry_t *find_app_by_name(const char *name)
{
    for (int i = 0; i < CONFIG_AKIRA_APP_MAX_INSTALLED; i++)
    {
        if (g_registry[i].name[0] != '\0' &&
            strcmp(g_registry[i].name, name) == 0)
        {
            return &g_registry[i];
        }
    }
    return NULL;
}

static app_entry_t *find_free_slot(void)
{
    for (int i = 0; i < CONFIG_AKIRA_APP_MAX_INSTALLED; i++)
    {
        if (g_registry[i].name[0] == '\0')
        {
            return &g_registry[i];
        }
    }
    return NULL;
}

static int validate_wasm(const void *binary, size_t size)
{
    if (size < sizeof(WASM_MAGIC))
    {
        return -EINVAL;
    }

    if (memcmp(binary, WASM_MAGIC, sizeof(WASM_MAGIC)) != 0)
    {
        LOG_ERR("Invalid WASM magic");
        return -EINVAL;
    }

    return 0;
}

static int save_app_binary(const char *name, const void *binary, size_t size)
{
    /* CRITICAL DEBUG LOGS */
    // LOG_INF("=== SAVE_APP_BINARY DEBUG ===");
    // LOG_INF("name: %s", name ? name : "NULL");
    // LOG_INF("binary ptr: %p", binary);
    // LOG_INF("size: %zu", size);
    
    if (!name) {
        LOG_ERR("INVALID: name is NULL");
        return -EINVAL;
    }
    
    if (!binary) {
        LOG_ERR("INVALID: binary pointer is NULL");
        return -EINVAL;
    }
    
    if (size == 0) {
        LOG_ERR("INVALID: size is zero");
        return -EINVAL;
    }
    
    /* Verify WASM magic bytes */
    const uint8_t *data = (const uint8_t *)binary;
    LOG_INF("First 4 bytes: 0x%02x 0x%02x 0x%02x 0x%02x", data[0], data[1], data[2], data[3]);
    if (size >= 4 && data[0] == 0x00 && data[1] == 0x61 && data[2] == 0x73 && data[3] == 0x6D) {
        LOG_INF("✅ Valid WASM magic number");
    } else {
        LOG_WRN("❌ Invalid WASM magic number");
    }
    
    /* Find ID for this app */
    app_entry_t *app = find_app_by_name(name);
    uint8_t id = app ? app->id : (g_app_count + 1);

    char path[APP_PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s/%03d_%s.wasm", APPS_DIR, id, name);
    // LOG_INF("Constructed path: %s", path);

    /* Use fs_manager to save (handles RAM fallback) */
    ssize_t written = fs_manager_write_file(path, binary, size);
    if (written < 0)
    {
        LOG_ERR("Failed to save %s: %zd", path, written);
        return written;
    }

    if ((size_t)written != size)
    {
        LOG_ERR("Failed to write app binary: wrote %zd of %zu", written, size);
        return -EIO;
    }

    LOG_INF("Saved app binary: %s (%zu bytes)", path, size);
    return 0;
}

static int delete_app_binary(const char *name)
{
    app_entry_t *app = find_app_by_name(name);
    if (!app)
    {
        return -ENOENT;
    }

    char path[APP_PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s/%03d_%s.wasm", APPS_DIR, app->id, name);

    /* Use fs_manager to delete (handles RAM storage too) */
    int ret = fs_manager_delete_file(path);
    if (ret < 0 && ret != -ENOENT)
    {
        LOG_ERR("Failed to delete %s: %d", path, ret);
        return ret;
    }

    /* Also delete app data directory */
    snprintf(path, sizeof(path), "%s/%s", APP_DATA_DIR, name);
    fs_manager_delete_file(path); /* Ignore error */

    return 0;
}

static void set_app_state(app_entry_t *app, app_state_t new_state)
{
    if (!app || app->state == new_state)
    {
        return;
    }

    app_state_t old_state = app->state;
    app->state = new_state;

    LOG_INF("App %s: %s -> %s", app->name,
            app_state_to_str(old_state), app_state_to_str(new_state));

    /* Notify callback */
    if (g_state_cb)
    {
        g_state_cb(app->id, old_state, new_state, g_state_cb_user);
    }

    /* Handle crash -> auto-restart */
    if (new_state == APP_STATE_ERROR && app->restart.enabled)
    {
        app->crash_count++;

        if (app->crash_count < app->restart.max_retries)
        {
            LOG_INF("Scheduling auto-restart for %s (attempt %d/%d)",
                    app->name, app->crash_count, app->restart.max_retries);

            strncpy(g_restart_app_name, app->name, APP_NAME_MAX_LEN);
            k_work_schedule(&g_restart_work, K_MSEC(app->restart.delay_ms));
        }
        else
        {
            LOG_ERR("App %s exceeded max restarts (%d), marking as FAILED",
                    app->name, app->restart.max_retries);
            app->state = APP_STATE_FAILED;
        }
    }
}

static void restart_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (g_restart_app_name[0] == '\0')
    {
        return;
    }

    LOG_INF("Auto-restarting app: %s", g_restart_app_name);
    app_manager_start(g_restart_app_name);
    g_restart_app_name[0] = '\0';
}
