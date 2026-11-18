/**
 * @file akira_app_containment.h
 * @brief Rules and helpers that keep any WASM app from hanging or crashing
 *        the system
 *
 * An app ends one of four ways: main() returns, it traps, it is stopped, or
 * its thread is aborted (a fault in its own thread, or a stop it ignored).
 * Only the first two run the app's own exit path. The runtime makes the other
 * two safe, provided every native follows these rules:
 *
 * - **Memory.** Validate every pointer *and its full length* against the
 *   app's linear memory (akira_wasm_ptr_to_native()) before taking any lock.
 *   WAMR's `*` signature checks one byte; `*~` checks the length that follows.
 * - **Waits.** Never wait forever, and never wait for an app-chosen time with
 *   a plain k_sleep(): use akira_runtime_sleep(), or wait in slices and give
 *   up once akira_runtime_stop_requested() is true. A stop wakes these waits.
 * - **Locks.** Hold a system lock (bus, flash, filesystem, driver, a lock of
 *   your own that a hook also takes) only inside an
 *   akira_native_critical_enter()/exit() span, take it with a bounded wait,
 *   and keep the span bounded: chunk large transfers. A stop never aborts a
 *   thread inside such a span, because the lock would be lost forever (it
 *   gives up with -EBUSY instead), but it does abort a thread that keeps
 *   spinning in WASM code or stays blocked outside a span.
 * - **Ownership.** Anything that outlives the call — a timer, an open file, a
 *   radio left transmitting, a held HID key, a callback into the app — is
 *   owned by the calling app's slot (akira_runtime_slot_of_exec_env()) and
 *   released by a hook registered with AKIRA_APP_RESOURCE_DEFINE(). The hook
 *   runs on every exit path, before the app's memory is freed.
 * - **No app pointers outside the call.** Callbacks and other threads must
 *   not keep pointers into app memory: copy through host buffers instead.
 *
 * A fault (CPU exception, k_oops(), a stack overflow caught by a hardware
 * stack guard) in an app's own thread, including inside a native, stops only
 * that app: the slot is reclaimed and the app manager reports it crashed. If
 * the fault is inside a critical span, the release hooks don't run (they
 * could block on the lock the thread held) and the slot stays parked until
 * reboot. A fault anywhere else, and a stack overflow only the sentinel or a
 * canary found, keeps Zephyr's policy.
 *
 * See docs/architecture/app-containment.md.
 *
 * @stability experimental
 * @since 1.7
 */

#ifndef AKIRA_APP_CONTAINMENT_H
#define AKIRA_APP_CONTAINMENT_H

#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- release hooks ------------------------------------------------------ */

/** One registered release hook. Create with AKIRA_APP_RESOURCE_DEFINE(). */
struct akira_app_resource {
    /** Descriptor id, used in diagnostics. */
    const char *name;
    /**
     * Optional. A stop was requested: wake whatever the app may be blocked
     * in that would not return on its own soon. Runs on the stopping thread
     * while the app may still be running; must not block.
     */
    void (*on_stop)(int slot, const char *app_name);
    /**
     * The app is gone, or about to be: stop and free everything @p slot
     * owns. Runs on the app's own thread, the stop path or the fault
     * cleanup, always before the app's memory is freed. Must be idempotent
     * and must touch only what @p slot owns.
     */
    void (*release)(int slot, const char *app_name);
};

#define Z_AKIRA_APP_RESOURCE_DEFINE(id, order, on_stop_fn, release_fn)             \
    const STRUCT_SECTION_ITERABLE(akira_app_resource,                              \
                                  akira_app_res_##order##_##id) = {                \
        STRINGIFY(id), (on_stop_fn), (release_fn)                                  \
    }

/**
 * @brief Register what to release when an app ends.
 *
 * @param id         C identifier, unique across the image.
 * @param order      Two-digit release order, lowest first (10 = subsystems
 *                   that write into app memory from other threads, 50 =
 *                   default; e.g. mesh 40 before RF 60).
 * @param on_stop_fn void (*)(int slot, const char *app_name), or NULL.
 * @param release_fn void (*)(int slot, const char *app_name), or NULL.
 */
#define AKIRA_APP_RESOURCE_DEFINE(id, order, on_stop_fn, release_fn)               \
    Z_AKIRA_APP_RESOURCE_DEFINE(id, order, on_stop_fn, release_fn)

/** Run every on_stop hook for @p slot. Called by the runtime. */
void akira_app_resources_on_stop_request(int slot, const char *app_name);

/** Run every release hook for @p slot. Called by the runtime; idempotent. */
void akira_app_resources_release(int slot, const char *app_name);

/* ---- helpers for natives ------------------------------------------------ */

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/** Slot of the app that made this native call, or -1. */
int akira_runtime_slot_of_exec_env(wasm_exec_env_t exec_env);

/** True once the calling app has been asked to stop: natives should bail out. */
bool akira_runtime_stop_requested(wasm_exec_env_t exec_env);

/**
 * Sleep on behalf of the calling app, waking early when the app is stopped.
 * Every native that waits for an app-chosen time uses this.
 *
 * @return true if woken by a stop request.
 */
bool akira_runtime_sleep(wasm_exec_env_t exec_env, k_timeout_t timeout);

/**
 * Bracket native work that holds a system lock. A stop never aborts the
 * app's thread inside such a span; keep the work bounded. Spans nest. A
 * fault inside a span parks the app's slot instead of reclaiming it.
 */
void akira_native_critical_enter(wasm_exec_env_t exec_env);
void akira_native_critical_exit(wasm_exec_env_t exec_env);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/** Slot whose app thread is @p tid, or -1 if it is not a WASM app thread. */
int akira_runtime_slot_of_thread(k_tid_t tid);

/**
 * Fatal-error policy for app threads. If the fault is in a WASM app's own
 * thread (thread context, not a kernel panic), queue the app's cleanup and
 * return true: the caller then returns and Zephyr aborts only that thread.
 * Every k_sys_fatal_error_handler in the tree calls this first.
 */
struct arch_esf;
bool akira_runtime_contain_fault(unsigned int reason, const struct arch_esf *esf);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_APP_CONTAINMENT_H */
