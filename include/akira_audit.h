/**
 * @file akira_audit.h
 * @brief Security audit trail: emit records, subscribe to them
 *
 * AkiraOS records security events (capability denials, watchdog kills,
 * integrity and signature results, app lifecycle) in a RAM ring. Product
 * firmware can add its own events with akira_audit_emit() and receive every
 * record through a sink, e.g. to persist, sign or upload it:
 *
 * @code
 * static void acme_sink(const struct akira_audit_record *rec, void *user)
 * {
 *     acme_queue_push(rec);   // copy and return; do the slow work elsewhere
 * }
 * AKIRA_AUDIT_SINK_DEFINE(acme_audit, acme_sink, NULL);
 * @endcode
 *
 * Sinks run on the system workqueue, in sequence order, never with the audit
 * lock held. They must not block. When records are overwritten before the
 * sinks ran (ring overflow), sinks get one AKIRA_AUDIT_DROPPED record whose
 * `detail` is the number of records lost; `seq` keeps counting across it.
 *
 * Requires CONFIG_AKIRA_SECURITY_AUDIT.
 *
 * @stability experimental
 * @since 1.7
 */

#ifndef AKIRA_AUDIT_H
#define AKIRA_AUDIT_H

#include <stdint.h>
#include <zephyr/sys/iterable_sections.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Core record types. Values are stable (they are exported off-device). */
enum akira_audit_type {
    AKIRA_AUDIT_SYSCALL_DENIED = 0,
    AKIRA_AUDIT_RATE_LIMITED = 1,
    AKIRA_AUDIT_WATCHDOG_KILL = 2,
    AKIRA_AUDIT_CAPABILITY_DENIED = 3,
    AKIRA_AUDIT_INTEGRITY_FAIL = 4,
    AKIRA_AUDIT_SIGNATURE_FAIL = 5,
    AKIRA_AUDIT_SIGNATURE_OK = 6,
    AKIRA_AUDIT_APP_LOADED = 7,
    AKIRA_AUDIT_APP_STARTED = 8,
    AKIRA_AUDIT_APP_STOPPED = 9,
    AKIRA_AUDIT_ADMISSION_DENIED = 10, /**< akira_admission.h refused an app */
    AKIRA_AUDIT_NET_DENIED = 11,       /**< akira_net_policy.h refused a request */

    /** First value for product-defined types. */
    AKIRA_AUDIT_VENDOR_FIRST = 0x1000,

    /** Synthetic: `detail` records were lost before the sinks ran. */
    AKIRA_AUDIT_DROPPED = 0xFFFF,
};

#define AKIRA_AUDIT_APP_NAME_LEN 32

/** One audit record as delivered to sinks. */
struct akira_audit_record {
    uint32_t seq;          /**< monotonically increasing from boot */
    uint32_t type;         /**< enum akira_audit_type or a vendor value */
    int64_t timestamp_ms;  /**< k_uptime_get() at the event */
    uint32_t detail;       /**< event-specific */
    char app_name[AKIRA_AUDIT_APP_NAME_LEN];
};

typedef void (*akira_audit_sink_fn)(const struct akira_audit_record *rec,
                                    void *user_data);

/** One registered sink. Create with AKIRA_AUDIT_SINK_DEFINE(). */
struct akira_audit_sink {
    akira_audit_sink_fn fn;
    void *user_data;
};

/** Register an audit sink. */
#define AKIRA_AUDIT_SINK_DEFINE(id, fn, user)                                      \
    const STRUCT_SECTION_ITERABLE(akira_audit_sink, id) = { (fn), (user) }

/**
 * @brief Record an audit event. ISR-safe; a no-op without
 *        CONFIG_AKIRA_SECURITY_AUDIT.
 *
 * @param type      enum akira_audit_type, or >= AKIRA_AUDIT_VENDOR_FIRST.
 * @param app_name  App (or subsystem) the event concerns, may be NULL.
 * @param detail    Event-specific value.
 */
void akira_audit_emit(uint32_t type, const char *app_name, uint32_t detail);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_AUDIT_H */
