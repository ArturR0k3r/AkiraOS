/**
 * @file app_manager.h
 * @brief AkiraOS App Manager - WASM Application Management
 *
 * Lightweight WebAssembly application management system built on OCRE.
 * Provides installation, lifecycle management, and resource control.
 *
 * Features:
 * - Multiple app sources: HTTP, BLE, USB, SD Card, Firmware
 * - App lifecycle: INSTALLED -> RUNNING -> STOPPED/ERROR/FAILED
 * - Auto-restart with configurable retries
 * - Persistent registry in LittleFS
 * - Optional manifest with defaults
 */

#ifndef AKIRA_APP_MANAGER_H
#define AKIRA_APP_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Configuration defaults */
#ifndef CONFIG_AKIRA_APP_MAX_INSTALLED
#define CONFIG_AKIRA_APP_MAX_INSTALLED 8
#endif

#ifndef CONFIG_AKIRA_APP_MAX_RUNNING
#define CONFIG_AKIRA_APP_MAX_RUNNING 2
#endif

#ifndef CONFIG_AKIRA_APP_MAX_SIZE_KB
#define CONFIG_AKIRA_APP_MAX_SIZE_KB 64
#endif

#ifndef CONFIG_AKIRA_APP_DEFAULT_HEAP_KB
#define CONFIG_AKIRA_APP_DEFAULT_HEAP_KB 16
#endif

#ifndef CONFIG_AKIRA_APP_DEFAULT_STACK_KB
#define CONFIG_AKIRA_APP_DEFAULT_STACK_KB 4
#endif

#ifndef CONFIG_AKIRA_APP_MAX_RETRIES
#define CONFIG_AKIRA_APP_MAX_RETRIES 3
#endif

#ifndef CONFIG_AKIRA_APP_RESTART_DELAY_MS
#define CONFIG_AKIRA_APP_RESTART_DELAY_MS 1000
#endif

#define APP_NAME_MAX_LEN 32
#define APP_VERSION_MAX_LEN 16
#define APP_PATH_MAX_LEN 64

    /**
     * @brief App state machine
     *
     * NEW       -> Being installed
     * INSTALLED -> Ready to run
     * RUNNING   -> Currently executing
     * STOPPED   -> Manually stopped
     * ERROR     -> Crashed, pending restart
     * FAILED    -> Exceeded max restart retries
     */
    typedef enum
    {
        APP_STATE_NEW = 0,
        APP_STATE_INSTALLED,
        APP_STATE_RUNNING,
        APP_STATE_STOPPED,
        APP_STATE_ERROR,
        APP_STATE_FAILED,
    } app_state_t;

    /**
     * @brief App source types
     */
    typedef enum
    {
        APP_SOURCE_UNKNOWN = 0,
        APP_SOURCE_HTTP,
        APP_SOURCE_BLE,
        APP_SOURCE_USB,
        APP_SOURCE_SD,
        APP_SOURCE_FIRMWARE,
    } app_source_t;

    /**
     * @brief App permissions (bitmask)
     */
    typedef enum
    {
        APP_PERM_NONE = 0,
        APP_PERM_GPIO = (1 << 0),
        APP_PERM_I2C = (1 << 1),
        APP_PERM_SPI = (1 << 2),
        APP_PERM_SENSOR = (1 << 3),
        APP_PERM_DISPLAY = (1 << 4),
        APP_PERM_STORAGE = (1 << 5),
        APP_PERM_NETWORK = (1 << 6),
        APP_PERM_BLE = (1 << 7),
        APP_PERM_RF = (1 << 8),
        APP_PERM_ALL = 0xFFFF,
    } app_permissions_t;

    /**
     * @brief App restart configuration
     */
    typedef struct
    {
        bool enabled;
        uint8_t max_retries;
        uint16_t delay_ms;
    } app_restart_config_t;

    /**
     * @brief App manifest (parsed from JSON or defaults)
     */
    typedef struct
    {
        char name[APP_NAME_MAX_LEN];
        char version[APP_VERSION_MAX_LEN];
        char entry[32];
        uint16_t heap_kb;
        uint16_t stack_kb;
        app_restart_config_t restart;
        uint16_t permissions;
    } app_manifest_t;

    /**
     * @brief App entry in registry
     */
    typedef struct
    {
        uint8_t id;
        char name[APP_NAME_MAX_LEN];
        char version[APP_VERSION_MAX_LEN];
        app_state_t state;
        app_source_t source;
        uint32_t size;
        uint16_t heap_kb;
        uint16_t stack_kb;
        uint16_t permissions;
        app_restart_config_t restart;
        uint8_t crash_count;
        int32_t container_id;  /* OCRE container ID, -1 if not loaded */
        uint32_t install_time; /* Unix timestamp */
        uint32_t last_start_time;
        bool is_preloaded; /* Firmware-embedded, cannot uninstall */
    } app_entry_t;

    /**
     * @brief App info for listing (public view)
     */
    typedef struct
    {
        uint8_t id;
        char name[APP_NAME_MAX_LEN];
        char version[APP_VERSION_MAX_LEN];
        app_state_t state;
        uint32_t size;
        uint16_t heap_kb;
        uint16_t stack_kb;
        uint8_t crash_count;
        bool auto_restart;
    } app_info_t;

    /**
     * @brief Install progress callback
     */
    typedef void (*app_install_progress_cb_t)(const char *name, size_t received,
                                              size_t total, void *user_data);

    /**
     * @brief Install complete callback
     */
    typedef void (*app_install_complete_cb_t)(const char *name, int result,
                                              void *user_data);

    /**
     * @brief App state change callback
     */
    typedef void (*app_state_change_cb_t)(uint8_t app_id, app_state_t old_state,
                                          app_state_t new_state, void *user_data);

    /* ===== Initialization ===== */

    /**
     * @brief Initialize the App Manager
     *
     * Loads registry from flash, initializes OCRE runtime,
     * and loads firmware-embedded apps.
     *
     * @return 0 on success, negative on error
     */
    int app_manager_init(void);

    /**
     * @brief Shutdown the App Manager
     *
     * Stops all running apps and saves registry.
     */
    void app_manager_shutdown(void);

    /* ===== Installation ===== */

    /**
     * @brief Install app from binary
     *
     * @param name App name (or NULL to use manifest/filename)
     * @param binary WASM binary data
     * @param size Binary size in bytes
     * @param manifest Optional manifest (NULL for defaults)
     * @param source Where the app came from
     * @return App ID (>= 0) on success, negative on error
     */
    int app_manager_install(const char *name, const void *binary, size_t size,
                            const app_manifest_t *manifest, app_source_t source);

    /**
     * @brief Install app from file path
     *
     * @param path Path to .wasm file (e.g., "/sd/apps/myapp.wasm")
     * @return App ID (>= 0) on success, negative on error
     */
    int app_manager_install_from_path(const char *path);

    /**
     * @brief Uninstall app
     *
     * Stops app if running, removes binary and registry entry.
     * Cannot uninstall preloaded (firmware) apps.
     *
     * @param name App name
     * @return 0 on success, negative on error
     */
    int app_manager_uninstall(const char *name);

    /* ===== Lifecycle ===== */

    /**
     * @brief Start an app
     *
     * @param name App name
     * @return 0 on success, negative on error
     */
    int app_manager_start(const char *name);

    /**
     * @brief Stop an app
     *
     * @param name App name
     * @return 0 on success, negative on error
     */
    int app_manager_stop(const char *name);

    /**
     * @brief Restart an app
     *
     * Resets crash counter and restarts.
     *
     * @param name App name
     * @return 0 on success, negative on error
     */
    int app_manager_restart(const char *name);

    /* ===== Query ===== */

    /**
     * @brief List all installed apps
     *
     * @param out_list Output array
     * @param max_count Maximum entries to return
     * @return Number of apps, negative on error
     */
    int app_manager_list(app_info_t *out_list, int max_count);

    /**
     * @brief Get app info by name
     *
     * @param name App name
     * @param out_info Output info structure
     * @return 0 on success, -ENOENT if not found
     */
    int app_manager_get_info(const char *name, app_info_t *out_info);

    /**
     * @brief Get app state
     *
     * @param name App name
     * @return App state, or APP_STATE_NEW if not found
     */
    app_state_t app_manager_get_state(const char *name);

    /**
     * @brief Get count of installed apps
     *
     * @return Number of installed apps
     */
    int app_manager_get_count(void);

    /**
     * @brief Get count of running apps
     *
     * @return Number of running apps
     */
    int app_manager_get_running_count(void);

    /* ===== Storage Scanning ===== */

    /**
     * @brief Scan directory for WASM apps
     *
     * Scans for *.wasm files and their manifests.
     *
     * @param path Directory path (e.g., "/sd/apps", "/usb/apps")
     * @param names Output array of app names found
     * @param max_count Maximum names to return
     * @return Number of apps found, negative on error
     */
    int app_manager_scan_dir(const char *path, char names[][APP_NAME_MAX_LEN],
                             int max_count);

    /* ===== App Loading Functions (Merged from app_loader) ===== */

    /**
     * @brief Load and install app from file
     *
     * Merged from legacy app_loader.c - loads WASM from file path.
     * This is equivalent to app_manager_install_from_path().
     *
     * @param path Path to .wasm file
     * @return App ID (>= 0) on success, negative on error
     */
    int app_load_from_file(const char *path);

    /* ===== Callbacks ===== */

    /**
     * @brief Register state change callback
     *
     * @param callback Callback function
     * @param user_data User data passed to callback
     */
    void app_manager_register_state_cb(app_state_change_cb_t callback,
                                       void *user_data);

    /* ===== HTTP API Support ===== */

    /**
     * @brief Begin chunked install (for HTTP/BLE upload)
     *
     * @param name App name
     * @param total_size Expected total size
     * @param source App source
     * @return Session handle (>= 0) on success, negative on error
     */
    int app_manager_install_begin(const char *name, size_t total_size,
                                  app_source_t source);

    /**
     * @brief Write chunk during install
     *
     * @param session Session handle from install_begin
     * @param data Chunk data
     * @param len Chunk length
     * @return 0 on success, negative on error
     */
    int app_manager_install_chunk(int session, const void *data, size_t len);

    /**
     * @brief Complete chunked install
     *
     * @param session Session handle
     * @param manifest Optional manifest (NULL for defaults)
     * @return App ID (>= 0) on success, negative on error
     */
    int app_manager_install_end(int session, const app_manifest_t *manifest);

    /**
     * @brief Abort chunked install
     *
     * @param session Session handle
     */
    void app_manager_install_abort(int session);

    /* ===== Manifest Helpers ===== */

    /**
     * @brief Parse manifest from JSON string
     *
     * @param json JSON string
     * @param json_len JSON length
     * @param out_manifest Output manifest
     * @return 0 on success, negative on error
     */
    int app_manifest_parse(const char *json, size_t json_len,
                           app_manifest_t *out_manifest);

    /**
     * @brief Initialize manifest with defaults
     *
     * @param manifest Manifest to initialize
     * @param name App name
     */
    void app_manifest_init_defaults(app_manifest_t *manifest, const char *name);

    /* ===== State helpers ===== */

    /**
     * @brief Get state name string
     *
     * @param state App state
     * @return State name (e.g., "RUNNING")
     */
    const char *app_state_to_str(app_state_t state);

    /**
     * @brief Get source name string
     *
     * @param source App source
     * @return Source name (e.g., "HTTP")
     */
    const char *app_source_to_str(app_source_t source);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_APP_MANAGER_H */
