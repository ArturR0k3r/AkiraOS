/**
 * @file akira_apps.h
 * @brief Install, list, start, stop and uninstall WASM apps
 *
 * The interface for product firmware and device-management clients. It wraps
 * the app manager without exposing its registry types. Every install goes
 * through admission (akira_admission.h).
 *
 * Calls may block on flash I/O and must not be made from a hook handler or an
 * ISR.
 *
 * @stability experimental
 * @since 1.7
 */

#ifndef AKIRA_APPS_H
#define AKIRA_APPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <akira_admission.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_AKIRA_APP_NAME_MAX_LEN
#define AKIRA_APPS_NAME_LEN CONFIG_AKIRA_APP_NAME_MAX_LEN
#else
#define AKIRA_APPS_NAME_LEN 32
#endif
#define AKIRA_APPS_VERSION_LEN 16

/** App state. Values match the app manager's app_state_t. */
enum akira_app_state {
    AKIRA_APP_STATE_NEW = 0,
    AKIRA_APP_STATE_INSTALLED,
    AKIRA_APP_STATE_RUNNING,
    AKIRA_APP_STATE_STOPPED,
    AKIRA_APP_STATE_ERROR,
    AKIRA_APP_STATE_FAILED,
};

/** One installed app. */
struct akira_app_summary {
    uint8_t id;      /**< registry id, 1-based */
    uint8_t state;   /**< enum akira_app_state */
    uint8_t source;  /**< enum akira_app_source */
    bool attested;   /**< a trusted signature covered it at install */
    uint8_t crash_count;
    uint32_t size;   /**< binary size, bytes */
    uint64_t cap_mask; /**< granted capabilities while loaded, else 0 */
    char name[AKIRA_APPS_NAME_LEN];
    char version[AKIRA_APPS_VERSION_LEN];
};

/** Number of installed apps, or negative errno. */
int akira_apps_count(void);

/**
 * @brief List installed apps.
 * @return number of entries written to @p out (at most @p max), or negative errno.
 */
int akira_apps_list(struct akira_app_summary *out, int max);

/** Look up one app by name. @return 0, -ENOENT, or negative errno. */
int akira_apps_get(const char *name, struct akira_app_summary *out);

/**
 * @brief Install an .akpkg package (gzip tar: manifest.json, app.wasm|app.aot,
 *        optional model.tflite and sig.* signatures).
 *
 * @param pkg        Package bytes.
 * @param len        Package length.
 * @param source     Who is installing it (admission providers see this).
 * @param name_out   Receives the installed app name; may be NULL.
 * @param name_size  Size of @p name_out.
 * @return registry id (> 0), -EACCES if admission refused it, or negative errno.
 */
int akira_apps_install_package(const uint8_t *pkg, size_t len,
                               enum akira_app_source source,
                               char *name_out, size_t name_size);

int akira_apps_start(const char *name);
int akira_apps_stop(const char *name);
int akira_apps_uninstall(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_APPS_H */
