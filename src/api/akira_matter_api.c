/**
 * @file akira_matter_api.c
 * @brief Matter/Thread WASM native API — bridges WASM apps to IPC transport
 *
 * Containment (akira_app_containment.h):
 * - Each IPC request holds the transport's TX mutex and an in-flight slot
 *   while it waits (<= AKIRA_MATTER_IPC_TIMEOUT_MS) for the co-processor,
 *   so it runs as a native critical section: a stop must not abort the app
 *   in there and leak them (three leaked slots and the bridge is dead).
 * - Event polls wait in short slices and return once the app is stopped.
 * - Fixed-size buffers behind a bare '*' (WAMR checks one byte) are
 *   validated for their full size, and outputs are written with memcpy (app
 *   pointers need not be aligned).
 * - Endpoints are owned by the app that added them: another app cannot
 *   report on them, and the ownership goes when the app does.
 */

#include "akira_matter_api.h"
#include <akira_app_containment.h>
#include <runtime/akira_matter_ipc.h>
#include <runtime/akira_runtime.h>
#include <runtime/security.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <wasm_export.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_matter_api, CONFIG_AKIRA_LOG_LEVEL);

/* Slice of an event wait: how quickly a stopped app gets out of a poll. */
#define MATTER_POLL_SLICE_MS 100

static atomic_t s_ipc_ready;
static K_MUTEX_DEFINE(s_ipc_init_lock);

/* Bit n = app slot n has used the bridge (see matter_release()). */
static ATOMIC_DEFINE(s_matter_used, AKIRA_MAX_WASM_INSTANCES);

#define MATTER_CRITICAL(env, expr)              \
    ({                                          \
        akira_native_critical_enter(env);       \
        int matter_crit_ret_ = (expr);          \
        akira_native_critical_exit(env);        \
        matter_crit_ret_;                       \
    })

/* Calling app's slot, recorded as a bridge user; -1 if not an app. */
static int matter_slot(wasm_exec_env_t env)
{
    int slot = akira_runtime_slot_of_exec_env(env);

    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        return -1;
    }
    atomic_set_bit(s_matter_used, slot);
    return slot;
}

/* Start the IPC bridge once.  Serialised: akira_matter_ipc_init() creates
 * the RX thread, and two apps starting it at once would create it twice. */
static int ensure_ipc(wasm_exec_env_t env)
{
    if (atomic_get(&s_ipc_ready)) {
        return 0;
    }

    int rc;

    akira_native_critical_enter(env);
    if (k_mutex_lock(&s_ipc_init_lock, K_MSEC(2 * AKIRA_MATTER_IPC_TIMEOUT_MS)) != 0) {
        rc = -EBUSY;
    } else {
        rc = atomic_get(&s_ipc_ready) ? 0 : akira_matter_ipc_init();
        if (rc == 0) {
            atomic_set(&s_ipc_ready, 1);
        }
        k_mutex_unlock(&s_ipc_init_lock);
    }
    akira_native_critical_exit(env);
    return rc;
}

/* --------------------------------------------------------------------------
 * Pointer validation
 *
 * Registration signatures (bottom of this file) must match these functions:
 *   matter_commission "($*)i"    passcode string, eui64_out[8]
 *   matter_send       "(**~)i"   eui64[8], payload + len (WAMR-validated)
 *   matter_subscribe  "(*i)i"    eui64[8], attr_id
 *   matter_poll       "(***~i)i" src_eui64[8], attr_id[4], buf + buf_len, timeout
 * A bare '*' only validates ONE byte, so the fixed-size buffers are checked
 * here for their full size.
 * ------------------------------------------------------------------------- */
static bool matter_span_ok(wasm_exec_env_t env, const void *ptr, uint32_t len)
{
    wasm_module_inst_t mi = wasm_runtime_get_module_inst(env);

    return mi && ptr && wasm_runtime_validate_native_addr(mi, (void *)ptr, (uint64_t)len);
}

/* strnlen() without relying on POSIX visibility. */
static size_t matter_strnlen(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

static int matter_ipc_err(int rc)
{
    return (rc == -ETIMEDOUT) ? MATTER_ERR_TIMEOUT : MATTER_ERR_IO;
}

/*
 * Wait for the next bridge event.  @p timeout_ms < 0 waits until an event
 * arrives, as before, but in MATTER_POLL_SLICE_MS slices that end as soon
 * as the app is stopped.  No lock is held while waiting.
 * Returns 0, -EAGAIN on timeout / stop, or another negative errno.
 */
static int matter_wait_event(wasm_exec_env_t env, struct akira_matter_event *evt,
                             int32_t timeout_ms)
{
    bool forever = timeout_ms < 0;
    uint32_t left = forever ? 0 : (uint32_t)timeout_ms;

    for (;;) {
        uint32_t slice = forever ? MATTER_POLL_SLICE_MS
                                 : MIN(left, (uint32_t)MATTER_POLL_SLICE_MS);
        int rc = akira_matter_ipc_poll(evt, slice ? K_MSEC(slice) : K_NO_WAIT);

        if (rc != -EAGAIN && rc != -ENOMSG) {
            return rc;
        }
        if (akira_runtime_stop_requested(env)) {
            return -EAGAIN;
        }
        if (!forever) {
            if (left <= slice) {
                return -EAGAIN;
            }
            left -= slice;
        }
    }
}

/* --------------------------------------------------------------------------
 * matter_commission
 * ------------------------------------------------------------------------- */
int akira_native_matter_commission(wasm_exec_env_t exec_env,
                                   const char *passcode,
                                   uint8_t *eui64_out)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    /* "$" proved passcode is a terminated string in app memory; the frame
     * carries at most AKIRA_MATTER_IPC_PASSCODE_LEN - 1 characters. */
    if (!passcode || !eui64_out ||
        matter_strnlen(passcode, AKIRA_MATTER_IPC_PASSCODE_LEN) >= AKIRA_MATTER_IPC_PASSCODE_LEN ||
        !matter_span_ok(exec_env, eui64_out, AKIRA_MATTER_IPC_EUI64_LEN)) {
        return MATTER_ERR_INVALID;
    }
    if (matter_slot(exec_env) < 0) {
        return MATTER_ERR_NOPERM;
    }

    int rc = ensure_ipc(exec_env);
    if (rc != 0) {
        LOG_WRN_RATELIMIT("matter: IPC init failed (%d)", rc);
        return MATTER_ERR_NO_COPROC;
    }

    /* Copied out of app memory first; the result lands in a local and the
     * app's buffer is only written on success. */
    char code[AKIRA_MATTER_IPC_PASSCODE_LEN];
    uint8_t eui64[AKIRA_MATTER_IPC_EUI64_LEN] = {0};

    strncpy(code, passcode, sizeof(code) - 1);
    code[sizeof(code) - 1] = '\0';
    rc = MATTER_CRITICAL(exec_env, akira_matter_ipc_commission(code, eui64));
    memset(code, 0, sizeof(code));
    if (rc != 0) {
        LOG_WRN_RATELIMIT("matter: commission failed (%d)", rc);
        return matter_ipc_err(rc);
    }
    memcpy(eui64_out, eui64, sizeof(eui64));
    return MATTER_OK;
}

/* --------------------------------------------------------------------------
 * matter_send
 * ------------------------------------------------------------------------- */
int akira_native_matter_send(wasm_exec_env_t exec_env,
                             const uint8_t *eui64,
                             const uint8_t *payload,
                             uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!eui64 || !payload || len == 0 ||
        len > AKIRA_MATTER_IPC_MAX_PAYLOAD - AKIRA_MATTER_IPC_EUI64_LEN ||
        !matter_span_ok(exec_env, eui64, AKIRA_MATTER_IPC_EUI64_LEN) ||
        !matter_span_ok(exec_env, payload, len)) {
        return MATTER_ERR_INVALID;
    }
    if (matter_slot(exec_env) < 0) {
        return MATTER_ERR_NOPERM;
    }

    int rc = ensure_ipc(exec_env);
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    rc = MATTER_CRITICAL(exec_env, akira_matter_ipc_send(eui64, payload, (uint16_t)len));
    if (rc != 0) {
        return matter_ipc_err(rc);
    }
    return MATTER_OK;
}

/* --------------------------------------------------------------------------
 * matter_subscribe
 * ------------------------------------------------------------------------- */
int akira_native_matter_subscribe(wasm_exec_env_t exec_env,
                                  const uint8_t *eui64,
                                  int32_t attr_id)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!eui64 || attr_id < 0 || attr_id > 0xFFFF ||
        !matter_span_ok(exec_env, eui64, AKIRA_MATTER_IPC_EUI64_LEN)) {
        return MATTER_ERR_INVALID;
    }
    if (matter_slot(exec_env) < 0) {
        return MATTER_ERR_NOPERM;
    }

    int rc = ensure_ipc(exec_env);
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    rc = MATTER_CRITICAL(exec_env, akira_matter_ipc_subscribe(eui64, (uint16_t)attr_id));
    if (rc != 0) {
        return matter_ipc_err(rc);
    }
    return MATTER_OK;
}

/* --------------------------------------------------------------------------
 * matter_poll
 * ------------------------------------------------------------------------- */
int akira_native_matter_poll(wasm_exec_env_t exec_env,
                             uint8_t *src_eui64,
                             int32_t *attr_id_ptr,
                             uint8_t *buf,
                             uint32_t buf_len,
                             int32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!src_eui64 || !attr_id_ptr || !buf || buf_len == 0 ||
        !matter_span_ok(exec_env, src_eui64, AKIRA_MATTER_IPC_EUI64_LEN) ||
        !matter_span_ok(exec_env, attr_id_ptr, sizeof(int32_t)) ||
        !matter_span_ok(exec_env, buf, buf_len)) {
        return MATTER_ERR_INVALID;
    }
    if (matter_slot(exec_env) < 0) {
        return MATTER_ERR_NOPERM;
    }

    int rc = ensure_ipc(exec_env);
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    struct akira_matter_event evt;

    rc = matter_wait_event(exec_env, &evt, timeout_ms);
    if (rc == -EAGAIN) {
        return MATTER_ERR_TIMEOUT;
    }
    if (rc != 0) {
        return MATTER_ERR_IO;
    }

    int32_t attr = (int32_t)evt.attr_id;
    memcpy(src_eui64, evt.src_eui64, AKIRA_MATTER_IPC_EUI64_LEN);
    memcpy(attr_id_ptr, &attr, sizeof(attr));

    uint32_t copy = MIN(buf_len, (uint32_t)MIN(evt.value_len, (uint16_t)sizeof(evt.value)));
    if (copy > 0) {
        memcpy(buf, evt.value, copy);
    }
    return (int)copy;
}

#ifdef CONFIG_AKIRA_MATTER_ACCESSORY
/* --------------------------------------------------------------------------
 * Accessory direction — expose AkiraOS's own hardware as a Matter device.
 * ------------------------------------------------------------------------- */

#define MATTER_MAX_CLUSTERS 8
/* Endpoints tracked per app.  The co-processor has no "remove endpoint"
 * command, so this caps how many an app can create per run. */
#define MATTER_EP_PER_APP   8
#define MATTER_EP_TABLE     16

/* Endpoint -> owning app slot + 1 (0 = free entry). */
static struct {
    uint8_t ep;
    uint8_t owner1;
} s_ep_owner[MATTER_EP_TABLE];
static struct k_spinlock s_ep_lock;

static int ep_count_owned(int slot)
{
    int n = 0;

    for (int i = 0; i < MATTER_EP_TABLE; i++) {
        if (s_ep_owner[i].owner1 == (uint8_t)(slot + 1)) {
            n++;
        }
    }
    return n;
}

/* Record @p ep as @p slot's (the latest add of an endpoint id wins). */
static void ep_record(uint8_t ep, int slot)
{
    k_spinlock_key_t key = k_spin_lock(&s_ep_lock);
    int free_i = -1;

    for (int i = 0; i < MATTER_EP_TABLE; i++) {
        if (s_ep_owner[i].owner1 != 0 && s_ep_owner[i].ep == ep) {
            s_ep_owner[i].owner1 = (uint8_t)(slot + 1);
            k_spin_unlock(&s_ep_lock, key);
            return;
        }
        if (s_ep_owner[i].owner1 == 0 && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i >= 0) {
        s_ep_owner[free_i].ep = ep;
        s_ep_owner[free_i].owner1 = (uint8_t)(slot + 1);
    }
    k_spin_unlock(&s_ep_lock, key);
}

/* True unless @p ep belongs to another running app. */
static bool ep_may_use(uint8_t ep, int slot)
{
    k_spinlock_key_t key = k_spin_lock(&s_ep_lock);
    bool ok = true;

    for (int i = 0; i < MATTER_EP_TABLE; i++) {
        if (s_ep_owner[i].owner1 != 0 && s_ep_owner[i].ep == ep) {
            ok = (s_ep_owner[i].owner1 == (uint8_t)(slot + 1));
            break;
        }
    }
    k_spin_unlock(&s_ep_lock, key);
    return ok;
}

/* matter_endpoint_add — sig "(i*~)i": device_type, clusters, n_clusters */
int akira_native_matter_endpoint_add(wasm_exec_env_t exec_env,
                                     int32_t device_type,
                                     const uint32_t *clusters,
                                     uint32_t n_clusters)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    /* WAMR checked n_clusters BYTES; the array is n_clusters uint32s. */
    if (!clusters || device_type < 0 || device_type > 0xFFFF ||
        n_clusters == 0 || n_clusters > MATTER_MAX_CLUSTERS ||
        !matter_span_ok(exec_env, clusters, n_clusters * sizeof(uint32_t))) {
        return MATTER_ERR_INVALID;
    }
    int slot = matter_slot(exec_env);
    if (slot < 0) {
        return MATTER_ERR_NOPERM;
    }

    k_spinlock_key_t key = k_spin_lock(&s_ep_lock);
    int owned = ep_count_owned(slot);
    k_spin_unlock(&s_ep_lock, key);
    if (owned >= MATTER_EP_PER_APP) {
        return MATTER_ERR_INVALID;
    }

    int rc = ensure_ipc(exec_env);
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    /* Copied out: the app's array need not be aligned. */
    uint32_t ids[MATTER_MAX_CLUSTERS];
    memcpy(ids, clusters, n_clusters * sizeof(uint32_t));

    uint8_t endpoint = 0;
    rc = MATTER_CRITICAL(exec_env,
                         akira_matter_ipc_endpoint_add((uint16_t)device_type, ids,
                                                       (uint8_t)n_clusters, &endpoint));
    if (rc != 0) {
        return matter_ipc_err(rc);
    }
    ep_record(endpoint, slot);
    return (int)endpoint;
}

/* matter_report_attr — sig "(iii*~)i": endpoint, cluster, attr, val, len */
int akira_native_matter_report_attr(wasm_exec_env_t exec_env,
                                    int32_t endpoint, int32_t cluster, int32_t attr,
                                    const void *val, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!val || endpoint < 0 || endpoint > 0xFF || len == 0 || len > 0xFF) {
        return MATTER_ERR_INVALID;
    }
    int slot = matter_slot(exec_env);
    if (slot < 0 || !ep_may_use((uint8_t)endpoint, slot)) {
        return MATTER_ERR_NOPERM;
    }

    int rc = ensure_ipc(exec_env);
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    rc = MATTER_CRITICAL(exec_env,
                         akira_matter_ipc_report((uint8_t)endpoint, (uint32_t)cluster,
                                                 (uint32_t)attr, (const uint8_t *)val,
                                                 (uint16_t)len));
    if (rc != 0) {
        return matter_ipc_err(rc);
    }
    return MATTER_OK;
}

/* matter_cmd_poll — sig "(****~i)i": endpoint*, cluster*, cmd*, buf, buf_len, timeout */
int akira_native_matter_cmd_poll(wasm_exec_env_t exec_env,
                                 int32_t *endpoint, int32_t *cluster, int32_t *cmd,
                                 uint8_t *buf, uint32_t buf_len, int32_t timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!endpoint || !cluster || !cmd || !buf || buf_len == 0 ||
        !matter_span_ok(exec_env, endpoint, sizeof(int32_t)) ||
        !matter_span_ok(exec_env, cluster, sizeof(int32_t)) ||
        !matter_span_ok(exec_env, cmd, sizeof(int32_t)) ||
        !matter_span_ok(exec_env, buf, buf_len)) {
        return MATTER_ERR_INVALID;
    }
    if (matter_slot(exec_env) < 0) {
        return MATTER_ERR_NOPERM;
    }

    int rc = ensure_ipc(exec_env);
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    struct akira_matter_event evt;

    rc = matter_wait_event(exec_env, &evt, timeout_ms);
    if (rc == -EAGAIN) {
        return MATTER_ERR_TIMEOUT;
    }
    if (rc != 0) {
        return MATTER_ERR_IO;
    }
    /* This poll only serves accessory-direction commands. A controller-attr
     * event here means the app mixed both roles; drop it and report timeout. */
    if (evt.kind != AKIRA_MATTER_EVT_ACC_CMD) {
        return MATTER_ERR_TIMEOUT;
    }

    int32_t v = (int32_t)evt.endpoint_id;
    memcpy(endpoint, &v, sizeof(v));
    v = (int32_t)evt.cluster_id;
    memcpy(cluster, &v, sizeof(v));
    v = (int32_t)evt.cmd_id;
    memcpy(cmd, &v, sizeof(v));

    uint32_t copy = MIN(buf_len, (uint32_t)MIN(evt.value_len, (uint16_t)sizeof(evt.value)));
    if (copy > 0) {
        memcpy(buf, evt.value, copy);
    }
    return (int)copy;
}

/* matter_open_pairing — sig "(i)i": timeout_sec */
int akira_native_matter_open_pairing(wasm_exec_env_t exec_env, int32_t timeout_sec)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (timeout_sec < 0 || timeout_sec > 0xFFFF) {
        return MATTER_ERR_INVALID;
    }
    if (matter_slot(exec_env) < 0) {
        return MATTER_ERR_NOPERM;
    }

    int rc = ensure_ipc(exec_env);
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    rc = MATTER_CRITICAL(exec_env, akira_matter_ipc_open_pairing((uint16_t)timeout_sec));
    if (rc != 0) {
        return matter_ipc_err(rc);
    }
    return MATTER_OK;
}

/* matter_get_pairing — sig "(*~*~)i": qr, qr_len, manual, manual_len */
int akira_native_matter_get_pairing(wasm_exec_env_t exec_env,
                                    char *qr, uint32_t qr_len,
                                    char *manual, uint32_t manual_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MATTER, MATTER_ERR_NOPERM);

    if (!qr || qr_len == 0 || !manual || manual_len == 0) {
        return MATTER_ERR_INVALID;
    }
    if (matter_slot(exec_env) < 0) {
        return MATTER_ERR_NOPERM;
    }

    int rc = ensure_ipc(exec_env);
    if (rc != 0) { return MATTER_ERR_NO_COPROC; }

    /* Fetched into host buffers sized for the app's, then copied out: the
     * app's memory is never written while the transport is mid-request. */
    char qr_l[AKIRA_MATTER_IPC_QR_LEN + 1];
    char man_l[AKIRA_MATTER_IPC_MANUAL_LEN + 1];
    size_t qn = MIN((size_t)qr_len, sizeof(qr_l));
    size_t mn = MIN((size_t)manual_len, sizeof(man_l));

    rc = MATTER_CRITICAL(exec_env, akira_matter_ipc_get_qr(qr_l, qn, man_l, mn));
    if (rc != 0) {
        return matter_ipc_err(rc);
    }
    memcpy(qr, qr_l, matter_strnlen(qr_l, qn - 1) + 1);
    memcpy(manual, man_l, matter_strnlen(man_l, mn - 1) + 1);
    return MATTER_OK;
}
#endif /* CONFIG_AKIRA_MATTER_ACCESSORY */

/* --------------------------------------------------------------------------
 * App teardown
 * ------------------------------------------------------------------------- */

/* The app is gone: its endpoints are no longer reserved for it, and once no
 * app uses the bridge, events queued for the departed are not handed to the
 * next one.  The co-processor protocol has no unsubscribe / remove-endpoint
 * command, so its subscriptions and endpoints stay on the co-processor. */
static void matter_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);

    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES ||
        !atomic_test_and_clear_bit(s_matter_used, slot)) {
        return;
    }

#ifdef CONFIG_AKIRA_MATTER_ACCESSORY
    k_spinlock_key_t key = k_spin_lock(&s_ep_lock);
    for (int i = 0; i < MATTER_EP_TABLE; i++) {
        if (s_ep_owner[i].owner1 == (uint8_t)(slot + 1)) {
            s_ep_owner[i].owner1 = 0;
        }
    }
    k_spin_unlock(&s_ep_lock, key);
#endif

    for (int i = 0; i < AKIRA_MAX_WASM_INSTANCES; i++) {
        if (atomic_test_bit(s_matter_used, i)) {
            return; /* another app still reads the queue */
        }
    }
    if (atomic_get(&s_ipc_ready)) {
        struct akira_matter_event evt;

        /* bounded: the queue is a few entries deep */
        for (int i = 0; i < 16; i++) {
            if (akira_matter_ipc_poll(&evt, K_NO_WAIT) != 0) {
                break;
            }
        }
    }
}

AKIRA_APP_RESOURCE_DEFINE(matter, 50, NULL, matter_release);

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md.
 * The controller signatures were corrected to match the SDK declarations and
 * these functions: the previous ones passed lengths where the C functions
 * expect pointers (and apps built with the SDK could not link them). */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_MATTER))
#include <akira_native_registry.h>

/* matter: Thread/Matter co-processor IPC bridge */
static const NativeSymbol akira_matter_natives[] = {
    {"matter_commission", (void *)akira_native_matter_commission, "($*)i",     NULL},
    {"matter_send",       (void *)akira_native_matter_send,       "(**~)i",    NULL},
    {"matter_subscribe",  (void *)akira_native_matter_subscribe,  "(*i)i",     NULL},
    {"matter_poll",       (void *)akira_native_matter_poll,       "(***~i)i",  NULL},
#ifdef CONFIG_AKIRA_MATTER_ACCESSORY
    {"matter_endpoint_add", (void *)akira_native_matter_endpoint_add, "(i*~)i",    NULL},
    {"matter_report_attr",  (void *)akira_native_matter_report_attr,  "(iii*~)i",  NULL},
    {"matter_cmd_poll",     (void *)akira_native_matter_cmd_poll,     "(****~i)i", NULL},
    {"matter_open_pairing", (void *)akira_native_matter_open_pairing, "(i)i",      NULL},
    {"matter_get_pairing",  (void *)akira_native_matter_get_pairing,  "(*~*~)i",   NULL},
#endif
};

AKIRA_NATIVE_API_DEFINE(akira_matter_api, "env", akira_matter_natives);
#endif
