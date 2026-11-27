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

/**
 * @brief True if @p name is a valid app name.
 *
 * App names become file names (/lfs/apps/NNN_<name>.wasm, the app's data
 * folder, its settings namespace and IPC topics), and FAT matches names
 * case-insensitively and drops trailing dots. So a name is 1 to
 * AKIRA_APPS_NAME_LEN - 1 characters from [a-z0-9_.-], starts with a letter or
 * digit, does not end with '.', and has no "..". Install refuses any other
 * name; nothing is truncated or rewritten.
 */
static inline bool akira_app_name_valid(const char *name)
{
    size_t n = 0;

    if (!name) {
        return false;
    }
    for (; n < AKIRA_APPS_NAME_LEN && name[n] != '\0'; n++) {
        char c = name[n];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');

        if (n == 0 ? !alnum : !(alnum || c == '_' || c == '-' || c == '.')) {
            return false;
        }
        if (c == '.' && name[n + 1] == '.') {
            return false;
        }
    }
    return n > 0 && n < AKIRA_APPS_NAME_LEN && name[n - 1] != '.';
}

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

/** Largest command or reply akira_apps_send_command() carries, bytes. */
#ifdef CONFIG_AKIRA_IPC_MSG_MAX_SIZE
#define AKIRA_APPS_CMD_MAX CONFIG_AKIRA_IPC_MSG_MAX_SIZE
#else
#define AKIRA_APPS_CMD_MAX 256
#endif

/** Room for an app's manifest "commands" array (raw JSON). */
#define AKIRA_APPS_COMMANDS_LEN 512

/**
 * @brief Copy the "commands" array from an app's manifest, as raw JSON.
 *
 * The array describes the commands the app accepts through
 * akira_apps_send_command(); its format is up to the app. Available while
 * the app is loaded: otherwise, or when the manifest has none, @p buf
 * receives "[]".
 *
 * @return 0, -ENOENT if no such app, -ENODEV, or -EINVAL.
 */
int akira_apps_get_commands(const char *name, char *buf, size_t size);

/**
 * @brief Send an app-defined command to a running app and wait for its reply.
 *
 * The command is published on the app's "<name>.cmd" IPC topic, which the
 * app subscribes to itself; the reply is read from "<name>.evt" (the
 * convention the BLE companion service uses). Command and reply are opaque
 * and at most AKIRA_APPS_CMD_MAX bytes. Blocks for up to @p timeout_ms, so
 * call it from a thread or work queue, never from a hook handler or the BT
 * thread. Needs CONFIG_AKIRA_WASM_IPC.
 *
 * @return reply length (>= 0),
 *         -ENOENT if the app is not running or not subscribed to its command topic,
 *         -ETIMEDOUT if it did not reply in time,
 *         -EMSGSIZE if @p cmd_len exceeds AKIRA_APPS_CMD_MAX,
 *         -ENOBUFS if the reply does not fit @p reply_size,
 *         -ENOTSUP without CONFIG_AKIRA_WASM_IPC, -ENODEV, or -EINVAL.
 */
int akira_apps_send_command(const char *name, const void *cmd, size_t cmd_len,
                            void *reply, size_t reply_size, uint32_t timeout_ms);

/**
 * @brief Read the newest state an app published on its "<name>.evt" topic.
 *
 * The first call for an app subscribes a system reader to that topic, so only
 * messages published after it are seen. Each call drains what arrived since the
 * last one and returns the newest message: the topic carries the app's state
 * and command replies (the app_cmd_bridge convention), so the latest one wins.
 * A message longer than @p size is cut to it. Never blocks. Needs
 * CONFIG_AKIRA_WASM_IPC.
 *
 * @return length of the newest message (> 0), 0 if nothing new,
 *         -ENOTSUP without CONFIG_AKIRA_WASM_IPC, -ENOMEM, -ENAMETOOLONG,
 *         or -EINVAL.
 */
int akira_apps_state_read(const char *name, void *buf, size_t size);

/**
 * @brief Subscribe the reader for @p name without reading, so messages the app
 * publishes from now on are kept for akira_apps_state_read() (e.g. before the
 * app starts). @return 0, -ENOTSUP, -ENOMEM, -ENAMETOOLONG, or -EINVAL.
 */
int akira_apps_state_watch(const char *name);

/** @brief Drop the reader akira_apps_state_read() subscribed for @p name. */
void akira_apps_state_forget(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_APPS_H */
