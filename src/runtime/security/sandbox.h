/**
 * @file sandbox.h
 * @brief AkiraOS Runtime Sandbox - Syscall Filtering & Rate Limiting
 *
 * Provides runtime sandboxing for WASM applications:
 * - Syscall filtering based on trust level and capabilities
 * - Rate limiting for resource-intensive operations
 * - Execution watchdog with configurable timeouts
 * - Security audit logging
 *
 * Design: Zero-allocation hot path using pre-computed bitmasks and
 * atomic counters. Target overhead: <100ns per syscall check.
 */

#ifndef AKIRA_SANDBOX_H
#define AKIRA_SANDBOX_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include "trust_levels.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Syscall Categories ===== */

/** Syscall category bitmask for filtering */
typedef enum {
    SYSCALL_CAT_DISPLAY     = (1U << 0),
    SYSCALL_CAT_INPUT       = (1U << 1),
    SYSCALL_CAT_SENSOR      = (1U << 2),
    SYSCALL_CAT_RF          = (1U << 3),
    SYSCALL_CAT_STORAGE     = (1U << 4),
    SYSCALL_CAT_NETWORK     = (1U << 5),
    SYSCALL_CAT_SYSTEM      = (1U << 6),
    SYSCALL_CAT_BLUETOOTH   = (1U << 7),
    SYSCALL_CAT_IPC         = (1U << 8),
    SYSCALL_CAT_MEMORY      = (1U << 9),
    SYSCALL_CAT_CRYPTO      = (1U << 10),
    SYSCALL_CAT_ALL         = 0x7FFU,
} sandbox_syscall_cat_t;

/* Default allowed syscall categories per trust level */
#define SANDBOX_TRUST_KERNEL_ALLOWED    SYSCALL_CAT_ALL
#define SANDBOX_TRUST_SYSTEM_ALLOWED    (SYSCALL_CAT_ALL & ~SYSCALL_CAT_SYSTEM)
#define SANDBOX_TRUST_TRUSTED_ALLOWED   (SYSCALL_CAT_DISPLAY | SYSCALL_CAT_INPUT | \
                                         SYSCALL_CAT_SENSOR | SYSCALL_CAT_RF | \
                                         SYSCALL_CAT_STORAGE | SYSCALL_CAT_NETWORK | \
                                         SYSCALL_CAT_BLUETOOTH | SYSCALL_CAT_IPC | \
                                         SYSCALL_CAT_MEMORY | SYSCALL_CAT_CRYPTO)
#define SANDBOX_TRUST_USER_ALLOWED      (SYSCALL_CAT_DISPLAY | SYSCALL_CAT_INPUT | \
                                         SYSCALL_CAT_SENSOR | SYSCALL_CAT_MEMORY)

/* ===== Rate Limiting ===== */

/** Rate limit bucket (token bucket algorithm) */
typedef struct {
    atomic_t tokens;            /**< Current tokens */
    uint16_t max_tokens;        /**< Maximum tokens (burst capacity) */
    uint16_t refill_per_sec;    /**< Tokens refilled per second */
    int64_t last_refill_ms;     /**< Last refill timestamp */
} sandbox_rate_bucket_t;

/** Rate limit configuration */
#define SANDBOX_RATE_DISPLAY_OPS    100  /**< Display ops per second */
#define SANDBOX_RATE_SENSOR_READS   50   /**< Sensor reads per second */
#define SANDBOX_RATE_RF_OPS         20   /**< RF operations per second */
#define SANDBOX_RATE_NETWORK_OPS    30   /**< Network ops per second */
#define SANDBOX_RATE_STORAGE_OPS    40   /**< Storage ops per second */
#define SANDBOX_RATE_IPC_OPS        200  /**< IPC messages per second */

#define SANDBOX_NUM_RATE_BUCKETS    6

/* ===== Execution Watchdog ===== */

/** Default execution timeout for WASM apps (ms) */
#ifndef CONFIG_AKIRA_SANDBOX_EXEC_TIMEOUT_MS
#define CONFIG_AKIRA_SANDBOX_EXEC_TIMEOUT_MS  5000
#endif

/** Maximum allowed execution timeout (ms) */
#define SANDBOX_MAX_EXEC_TIMEOUT_MS  30000

/* ===== Sandbox Context ===== */

/** Per-app sandbox context - pre-allocated in app slot */
typedef struct {
    /* Syscall filtering */
    uint32_t allowed_syscalls;          /**< Bitmask of allowed syscall categories */
    akira_trust_level_t trust_level;    /**< App trust level */

    /* Rate limiting */
    sandbox_rate_bucket_t rate_buckets[SANDBOX_NUM_RATE_BUCKETS];

    /* Execution watchdog */
    uint32_t exec_timeout_ms;           /**< Execution timeout in ms */
    int64_t exec_start_ms;              /**< Current execution start timestamp */
    bool exec_active;                   /**< Currently executing */

    /* Audit counters */
    uint32_t total_syscalls;            /**< Total syscall count */
    uint32_t denied_syscalls;           /**< Denied syscall count */
    uint32_t rate_limited_count;        /**< Rate-limited count */
    uint32_t watchdog_kills;            /**< Watchdog termination count */

    /* State */
    bool initialized;
} sandbox_ctx_t;

/* ===== Security Audit Event ===== */

/** Audit event types */
typedef enum {
    AUDIT_EVENT_SYSCALL_DENIED = 0,
    AUDIT_EVENT_RATE_LIMITED,
    AUDIT_EVENT_WATCHDOG_KILL,
    AUDIT_EVENT_CAPABILITY_DENIED,
    AUDIT_EVENT_INTEGRITY_FAIL,
    AUDIT_EVENT_SIGNATURE_FAIL,
    AUDIT_EVENT_SIGNATURE_OK,
    AUDIT_EVENT_APP_LOADED,
    AUDIT_EVENT_APP_STARTED,
    AUDIT_EVENT_APP_STOPPED,
    AUDIT_EVENT_MAX
} audit_event_type_t;

/** Audit log entry */
typedef struct {
    audit_event_type_t type;
    int64_t timestamp_ms;
    char app_name[32];
    uint32_t detail;                    /**< Event-specific detail (e.g., syscall cat) */
} audit_entry_t;

/** Audit log ring buffer size */
#ifndef CONFIG_AKIRA_AUDIT_LOG_SIZE
#define CONFIG_AKIRA_AUDIT_LOG_SIZE 32
#endif

/* ===== API Functions ===== */

/**
 * @brief Initialize the sandbox subsystem
 * @return 0 on success
 */
int sandbox_init(void);

/**
 * @brief Initialize sandbox context for an app
 *
 * @param ctx       Sandbox context to initialize
 * @param trust     Trust level for the app
 * @param cap_mask  Capability mask from manifest
 */
void sandbox_ctx_init(sandbox_ctx_t *ctx, akira_trust_level_t trust,
                      uint32_t cap_mask);

/**
 * @brief Check if a syscall is allowed (hot path)
 *
 * Performs category filter + rate limit check. Designed for
 * minimal overhead on the critical path.
 *
 * @param ctx       Sandbox context
 * @param category  Syscall category
 * @param app_name  App name (for audit logging)
 * @return true if allowed, false if denied
 */
bool sandbox_check_syscall(sandbox_ctx_t *ctx, sandbox_syscall_cat_t category,
                           const char *app_name);

/**
 * @brief Signal execution start (for watchdog)
 *
 * @param ctx  Sandbox context
 */
void sandbox_exec_begin(sandbox_ctx_t *ctx);

/**
 * @brief Signal execution end
 *
 * @param ctx  Sandbox context
 */
void sandbox_exec_end(sandbox_ctx_t *ctx);

/**
 * @brief Check if execution has timed out
 *
 * @param ctx  Sandbox context
 * @return true if timed out
 */
bool sandbox_exec_timed_out(sandbox_ctx_t *ctx);

/**
 * @brief Record a security audit event
 *
 * @param type      Event type
 * @param app_name  Application name
 * @param detail    Event-specific detail
 */
void sandbox_audit_log(audit_event_type_t type, const char *app_name,
                       uint32_t detail);

/**
 * @brief Get recent audit entries
 *
 * @param entries   Output buffer for entries
 * @param max_count Maximum entries to return
 * @return Number of entries returned
 */
int sandbox_audit_get_recent(audit_entry_t *entries, int max_count);

/**
 * @brief Get sandbox statistics for an app
 *
 * @param ctx  Sandbox context
 * @param buf  Output string buffer
 * @param len  Buffer length
 * @return Number of bytes written
 */
int sandbox_get_stats(const sandbox_ctx_t *ctx, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SANDBOX_H */
