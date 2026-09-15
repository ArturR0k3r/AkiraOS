/**
 * @file akira_hooks.h
 * @brief System event hooks for firmware and product extensions
 *
 * A handler defined with AKIRA_HOOK_DEFINE() is called synchronously when a
 * system event occurs — an app is installed or crashes, connectivity comes up,
 * an OTA update is staged. Product firmware uses this instead of editing the
 * boot sequence or guessing SYS_INIT priorities:
 *
 * @code
 * static void on_event(const struct akira_hook_event *e, void *user)
 * {
 *     if (e->type == AKIRA_HOOK_BOOT_READY) {
 *         acme_start();
 *     }
 * }
 * AKIRA_HOOK_DEFINE(acme_hook, AKIRA_HOOK_MASK(AKIRA_HOOK_BOOT_READY), on_event, NULL);
 * @endcode
 *
 * Handlers run on the thread that raised the event (see docs/api-reference/hooks.md
 * for the thread and stack of each event) and must not block or call back into
 * the subsystem that raised the event.
 *
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_HOOKS_H
#define AKIRA_HOOKS_H

#include <stdint.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/** System event types. */
enum akira_hook_event_type {
    /** After HAL and connectivity init, before the WASM runtime starts. */
    AKIRA_HOOK_BOOT_PRE_RUNTIME = 0,
    /** After the app manager is up; the system is ready to run apps. */
    AKIRA_HOOK_BOOT_READY,

    /** An app was installed. `app.name`, `app.registry_id`, `app.version`. */
    AKIRA_HOOK_APP_INSTALLED,
    /** An app was uninstalled. `app.name`. */
    AKIRA_HOOK_APP_UNINSTALLED,
    /** An app started. `app.name`, `app.registry_id`, `app.container_id`. */
    AKIRA_HOOK_APP_STARTED,
    /** An app stopped (explicit stop or clean exit). `app.name`, `app.exit_code`. */
    AKIRA_HOOK_APP_STOPPED,
    /** An app crashed or trapped (non-zero exit). `app.name`, `app.exit_code`. */
    AKIRA_HOOK_APP_CRASHED,
    /** An app exhausted its restart budget and is now FAILED. `app.name`. */
    AKIRA_HOOK_APP_FAILED,

    /** A network link came up. `net.link`. */
    AKIRA_HOOK_NET_UP,
    /** A network link went down. `net.link`. */
    AKIRA_HOOK_NET_DOWN,
    /** A Bluetooth central connected. */
    AKIRA_HOOK_BT_CONNECTED,
    /** A Bluetooth central disconnected. */
    AKIRA_HOOK_BT_DISCONNECTED,
    /** USB was configured by the host. */
    AKIRA_HOOK_USB_CONFIGURED,
    /** USB was disconnected. */
    AKIRA_HOOK_USB_DISCONNECTED,

    /** An OTA download started. */
    AKIRA_HOOK_OTA_STARTED,
    /** An OTA image was verified and staged for the next boot. */
    AKIRA_HOOK_OTA_STAGED,
    /** About to reboot into a staged OTA image. Last chance to persist state. */
    AKIRA_HOOK_OTA_PRE_REBOOT,
    /** The running image was confirmed after an OTA. */
    AKIRA_HOOK_OTA_CONFIRMED,
    /** An OTA failed or rolled back. `ota.error`. */
    AKIRA_HOOK_OTA_ERROR,

    AKIRA_HOOK_EVENT_COUNT
};

/** Network link kind for AKIRA_HOOK_NET_UP / _DOWN. */
enum akira_hook_net_link {
    AKIRA_HOOK_NET_LINK_UNKNOWN = 0,
    AKIRA_HOOK_NET_LINK_WIFI,
    AKIRA_HOOK_NET_LINK_ETHERNET,
    AKIRA_HOOK_NET_LINK_THREAD,
};

/** Event passed to a hook handler. The active union member follows `type`. */
struct akira_hook_event {
    enum akira_hook_event_type type;
    union {
        struct {
            const char *name;
            int registry_id;  /**< app registry id, or -1 */
            int container_id; /**< runtime slot id, or -1 */
            const char *version;
            int exit_code;
        } app;
        struct {
            enum akira_hook_net_link link;
        } net;
        struct {
            int state; /**< akira_ota_state_t */
            int error; /**< errno, 0 if none */
        } ota;
    };
};

/** Bit for one event type, for the AKIRA_HOOK_DEFINE() mask. */
#define AKIRA_HOOK_MASK(event_type) BIT64(event_type)
/** Every event. */
#define AKIRA_HOOK_MASK_ALL         (BIT64(AKIRA_HOOK_EVENT_COUNT) - 1U)

/** Handler signature. */
typedef void (*akira_hook_handler_t)(const struct akira_hook_event *event, void *user_data);

/** One registered handler. Create with AKIRA_HOOK_DEFINE(). */
struct akira_hook {
    uint64_t event_mask;
    akira_hook_handler_t handler;
    void *user_data;
};

/**
 * @brief Register a hook handler.
 *
 * @param id         C identifier for the hook, unique across the image.
 * @param mask       Events to receive, e.g. AKIRA_HOOK_MASK(AKIRA_HOOK_BOOT_READY).
 * @param handler_fn Handler function.
 * @param user       Opaque pointer passed to the handler.
 */
#define AKIRA_HOOK_DEFINE(id, mask, handler_fn, user)                              \
    const STRUCT_SECTION_ITERABLE(akira_hook, id) = {                              \
        (mask), (handler_fn), (user)                                               \
    }

/**
 * @brief Raise an event, calling every handler subscribed to its type.
 *
 * Called by AkiraOS subsystems. Runs handlers synchronously on the caller's
 * thread, in link order.
 */
void akira_hooks_emit(const struct akira_hook_event *event);

/** Convenience: emit an app-lifecycle event. */
void akira_hooks_emit_app(enum akira_hook_event_type type, const char *name,
                          int registry_id, int container_id, const char *version,
                          int exit_code);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_HOOKS_H */
