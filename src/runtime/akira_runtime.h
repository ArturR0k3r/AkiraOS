/**
 * @file akira_runtime.h
 * @brief AkiraOS Runtime - WASM Micro Runtime Integration
 *
 * Provides a simplified interface to the WASM Micro Runtime (WAMR).
 * @stability stable
 * @since 1.3
 */

#ifndef AKIRA_RUNTIME_H
#define AKIRA_RUNTIME_H

/* Minimal public header for the unified Akira runtime.
 * Per Minimalist Architecture this header only includes Zephyr basics and
 * the WAMR export header (no heavy dependencies here).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Optional WAMR dependency - provide minimal typedefs if runtime is disabled */
#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#else
typedef void *wasm_exec_env_t;
typedef void *wasm_module_inst_t;
#endif

#include <stdint.h>
#include <stdbool.h>
#include <runtime/security/sandbox.h>
#include <runtime/runtime_cache.h>
#include <runtime/security/trust_levels.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AKIRA_MAX_WASM_INSTANCES
#ifdef CONFIG_MAX_CONTAINERS
#define AKIRA_MAX_WASM_INSTANCES CONFIG_MAX_CONTAINERS
#else
#define AKIRA_MAX_WASM_INSTANCES 8
#endif
#endif

/**
 * @brief WASM app lifecycle status
 *
 * CREATED  → RUNNING (thread spawned, sem_start posted)
 * RUNNING  → EXITED  (thread returned from wasm_runtime_call_wasm)
 * EXITED   → STOPPED (k_thread_join() completed, resources freed)
 * Any      → ERROR   (instantiation failed, exception, or terminate timeout)
 */
typedef enum {
    AKIRA_APP_STATUS_CREATED = 0, /**< Loaded, not yet started */
    AKIRA_APP_STATUS_RUNNING = 1, /**< Thread is executing WASM */
    AKIRA_APP_STATUS_EXITED  = 2, /**< WASM returned, thread finishing cleanup */
    AKIRA_APP_STATUS_STOPPED = 3, /**< Thread joined, slot can be reused */
    AKIRA_APP_STATUS_ERROR   = 4, /**< Fatal error during start or execution */
} akira_app_status_t;

/* Internal managed app structure */
typedef struct {
    bool used;
    char name[32];
    wasm_module_t module;
    wasm_module_inst_t instance;
    wasm_exec_env_t exec_env;

    /* Lifecycle: status replaces old bool running */
    uint8_t status;           /**< akira_app_status_t */
    int8_t exit_code;         /**< Last exit code from WASM main() */
    bool stop_requested;      /**< akira_runtime_stop() asked WAMR to unwind this slot */

    /* Thread synchronization */
    k_tid_t tid;              /**< Thread ID (NULL if not running) */
    struct k_sem sem_start;   /**< Posted when WAMR instance is ready */
    struct k_mutex exit_mutex;  /**< Per-slot mutex for cond_exit wait — isolates stop() calls across slots */
    struct k_condvar cond_exit; /**< Broadcast when thread exits */

    uint64_t cap_mask;        /**< capability bitmask from manifest */
    uint32_t memory_quota;    /**< memory quota from manifest (0 = unlimited) */
    atomic_t memory_used;     /**< current memory usage (bytes) — updated atomically */

    /* Security: sandbox context for syscall filtering + rate limiting */
    sandbox_ctx_t sandbox;
    akira_trust_level_t trust_level;

    /* Performance: per-instance execution statistics */
    runtime_perf_stats_t perf;

    /* Cache: SHA-256 hash of loaded binary for module cache */
    uint8_t binary_hash[32];
    bool hash_valid;

    /* WAMR string lifetime: WAMR stores raw pointers into the binary buffer
     * for export/import name strings when wasm_binary_freeable=false (the
     * default). This buffer MUST stay alive until wasm_runtime_unload(). */
    uint8_t *wasm_binary;
} akira_managed_app_t;

/**
 * @brief Callback invoked when a WASM app thread exits
 *
 * Called from the app thread just before it terminates.
 * Safe to call akira_runtime_stop() from within this callback.
 *
 * @param slot      App slot index
 * @param exit_code Return value from WASM main() (0 = success)
 */
typedef void (*akira_runtime_exit_cb_t)(int slot, int exit_code);

/**
 * @brief Register a global exit callback
 *
 * Only one callback is supported. Pass NULL to deregister.
 * Typically called by app_manager during init.
 *
 * @param cb Callback function, or NULL
 */
void akira_runtime_set_exit_callback(akira_runtime_exit_cb_t cb);

/* Initialize the unified runtime (WAMR + storage + native API registration).
 * Returns 0 on success.
 */
int akira_runtime_init(void);

/* Load a WASM binary from memory into the runtime; returns an instance id
 * (>=0) or negative on error.
 */
int akira_runtime_load_wasm(const uint8_t *buffer, uint32_t size);

/* Start/stop a loaded instance by id */
int akira_runtime_start(int instance_id);
int akira_runtime_stop(int instance_id);

/* Persistent installation: save binary and optional manifest to /lfs/apps
 * If manifest_json is NULL, no manifest is saved. Returns instance id or negative on error.
 */
int akira_runtime_install_with_manifest(const char *name, const void *binary, size_t size, const char *manifest_json, size_t manifest_size);
int akira_runtime_install(const char *name, const void *binary, size_t size);

/* Destroy/uninstall helpers */
int akira_runtime_destroy(int instance_id);
int akira_runtime_uninstall(const char *name, int instance_id);

/* Capability guard helpers */
bool akira_security_check_exec(wasm_exec_env_t exec_env, uint64_t capability);
bool akira_security_check_native(uint64_t capability);

/*
 * WASM Memory Allocation with Per-App Quota Enforcement
 *
 * These functions provide quota-aware memory allocation for WASM apps.
 * Use PSRAM-preferred allocation and enforce per-app memory limits.
 * Quota violations return NULL gracefully without crashing.
 */

/**
 * @brief Allocate memory for a WASM app with quota enforcement
 *
 * @param exec_env  WAMR execution environment
 * @param size      Number of bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure/quota exceeded
 */
void *akira_wasm_malloc(wasm_exec_env_t exec_env, size_t size);

/**
 * @brief Free memory allocated with akira_wasm_malloc
 *
 * @param exec_env  WAMR execution environment
 * @param ptr       Pointer to memory
 */
void akira_wasm_free(wasm_exec_env_t exec_env, void *ptr);

/**
 * @brief Get current memory usage for an app
 *
 * @param instance_id  App instance ID
 * @return Current memory usage in bytes
 */
uint32_t akira_runtime_get_memory_used(int instance_id);

/**
 * @brief Get memory quota for an app
 *
 * @param instance_id  App instance ID
 * @return Memory quota in bytes (0 = unlimited)
 */
uint32_t akira_runtime_get_memory_quota(int instance_id);

/**
 * @brief Get sandbox context for an app (for API-level syscall checks)
 *
 * @param instance_id  App instance ID
 * @return Pointer to sandbox context, or NULL if invalid
 */
sandbox_ctx_t *akira_runtime_get_sandbox(int instance_id);

/**
 * @brief Get performance statistics for an app
 *
 * @param instance_id  App instance ID
 * @return Pointer to perf stats, or NULL if invalid
 */
runtime_perf_stats_t *akira_runtime_get_perf_stats(int instance_id);

/**
 * @brief Verify WASM binary integrity before loading
 *
 * Performs structural integrity check and optional signature verification.
 * Should be called before akira_runtime_load_wasm() for untrusted binaries.
 *
 * @param binary    WASM binary data
 * @param size      Binary size
 * @param hash_out  Output: SHA-256 hash (32 bytes), can be NULL
 * @return 0 on success, negative on error
 */
int akira_runtime_verify_binary(const uint8_t *binary, uint32_t size,
                                uint8_t *hash_out);

/**
 * @brief Get module cache statistics
 *
 * @param stats  Output statistics struct
 */
void akira_runtime_get_cache_stats(module_cache_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_RUNTIME_H */
