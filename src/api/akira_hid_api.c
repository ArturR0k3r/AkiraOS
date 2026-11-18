/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_hid_api
#include <zephyr/logging/log.h>
#include <crystal/akira_seal.h>
LOG_MODULE_REGISTER(akira_hid_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_hid_api.c
 * @brief HID native API wrappers for WASM apps
 *
 * Exports keyboard, gamepad, mouse, consumer media key, raw report, and
 * named action hotkey functions to WASM.  All functions require
 * AKIRA_CAP_HID in the calling app's capability mask.
 *
 * Guarded by CONFIG_AKIRA_WASM_HID at registration time (export_api.c).
 * Individual functions also guard on CONFIG_AKIRA_HID so stubless builds
 * return -ENOTSUP gracefully.
 *
 * Containment (akira_app_containment.h): every HID manager call holds the
 * manager mutex and may wait on the transport, so it runs as a native
 * critical section.  What an app leaves behind on the host — held keys and
 * buttons, a latched mouse button, its named actions, queued OUT reports,
 * a changed transport — is undone by hid_release() however the app ends.
 */

#include "akira_hid_api.h"
#include "akira_wasm_mem.h"
#include <akira_app_containment.h>
#include <runtime/akira_runtime.h>
#include <runtime/security.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <string.h>
#include <lib/mem_helper.h>

#ifdef CONFIG_AKIRA_HID
#include <connectivity/hid/hid_manager.h>
#include <connectivity/usb/usb_hid.h>
#endif

/* ── Polled receive queues for raw (ID 3) and FIDO (ID 4) channels ──────── */
/*
 * These queues are filled from USB interrupt context by callbacks registered
 * on the first hid_init(USB, …) call, and drained by WASM via hid_raw_recv /
 * hid_fido_recv in the app's polling loop.
 */
#ifdef CONFIG_AKIRA_HID

#define HID_RAW_QUEUE_DEPTH  4
/* CTAPHID messages reassemble across up to ~18 packets (1024-byte max
 * payload / 59 bytes per continuation frame). Windows bursts all
 * continuation packets within ~10ms of each other, far faster than the
 * WASM app's poll cadence drains them — a shallow queue silently drops
 * trailing packets and reassembly stalls forever. */
#define HID_FIDO_QUEUE_DEPTH 20

/* Largest report hid_send_raw_report() accepts. */
#define HID_RAW_REPORT_MAX 64

struct hid_raw_pkt  { uint8_t data[USB_HID_RAW_PAYLOAD_SIZE];  uint8_t len; };
struct hid_fido_pkt { uint8_t data[USB_HID_FIDO_PAYLOAD_SIZE]; uint8_t len; };

/* Ring buffers in PSRAM.  The report callbacks below are the only producers;
 * they may run in interrupt context, which is fine — the one window where
 * PSRAM is unreachable (cache off, during an internal-flash write) also has
 * interrupts disabled, so no ISR can observe it. */
AKIRA_BULK_MSGQ_DEFINE(hid_raw_msgq,  sizeof(struct hid_raw_pkt),
                       HID_RAW_QUEUE_DEPTH,  4);
AKIRA_BULK_MSGQ_DEFINE(hid_fido_msgq, sizeof(struct hid_fido_pkt),
                       HID_FIDO_QUEUE_DEPTH, 4);

static void hid_raw_isr_cb(const uint8_t *data, uint8_t len)
{
    struct hid_raw_pkt pkt;
    uint8_t copy = (len > USB_HID_RAW_PAYLOAD_SIZE) ? USB_HID_RAW_PAYLOAD_SIZE : len;
    memcpy(pkt.data, data, copy);
    pkt.len = copy;
    k_msgq_put(&hid_raw_msgq, &pkt, K_NO_WAIT); /* drop if full */
}

static void hid_fido_isr_cb(const uint8_t *data, uint8_t len)
{
    struct hid_fido_pkt pkt;
    uint8_t copy = (len > USB_HID_FIDO_PAYLOAD_SIZE) ? USB_HID_FIDO_PAYLOAD_SIZE : len;
    memcpy(pkt.data, data, copy);
    pkt.len = copy;
    k_msgq_put(&hid_fido_msgq, &pkt, K_NO_WAIT);
}

static void hid_maybe_register_usb_handlers(void)
{
    /* The raw/FIDO report handlers live in usb_hid.c, which is built only
     * with CONFIG_AKIRA_USB_HID. */
#ifdef CONFIG_AKIRA_USB_HID
    static atomic_t hid_usb_handlers_registered;

    if (atomic_cas(&hid_usb_handlers_registered, 0, 1))
    {
        usb_hid_raw_set_handler(hid_raw_isr_cb);
        usb_hid_fido_set_handler(hid_fido_isr_cb);
    }
#endif
}

/* ── Per-app bookkeeping for teardown ───────────────────────────────────── */
/* Bit n = app slot n used that part of HID, so hid_release() only touches
 * (and only sends reports for) what the departing app actually used. */
static ATOMIC_DEFINE(g_hid_used_slots, AKIRA_MAX_WASM_INSTANCES); /* state-changing call */
static ATOMIC_DEFINE(g_hid_cfg_slots, AKIRA_MAX_WASM_INSTANCES);  /* transport/types/enable */

/* HID configuration before the first app changed it; restored when the last
 * app that changed it is gone. */
static struct {
    bool valid;
    bool enabled;
    hid_transport_t transport;
    hid_device_type_t types;
} g_hid_saved;
static struct k_spinlock g_hid_saved_lock;

static int hid_slot(wasm_exec_env_t exec_env)
{
    int slot = akira_runtime_slot_of_exec_env(exec_env);

    return (slot >= 0 && slot < AKIRA_MAX_WASM_INSTANCES) ? slot : -1;
}

static bool hid_cfg_any(void)
{
    for (int i = 0; i < AKIRA_MAX_WASM_INSTANCES; i++) {
        if (atomic_test_bit(g_hid_cfg_slots, i)) {
            return true;
        }
    }
    return false;
}

/* Note that the calling app used HID (keys, reports, actions...). */
static void hid_mark_used(wasm_exec_env_t exec_env)
{
    int slot = hid_slot(exec_env);

    if (slot >= 0) {
        atomic_set_bit(g_hid_used_slots, slot);
    }
}

/* The calling app is about to change the HID configuration: remember the
 * system's configuration the first time any app does. */
static void hid_mark_config(wasm_exec_env_t exec_env)
{
    int slot = hid_slot(exec_env);
    const hid_state_t *st = hid_manager_get_state();
    bool enabled = st ? st->enabled : false;
    hid_transport_t transport = st ? st->transport : HID_TRANSPORT_NONE;
    hid_device_type_t types = st ? st->device_type : HID_DEVICE_KEYBOARD;
    k_spinlock_key_t key = k_spin_lock(&g_hid_saved_lock);

    if (!g_hid_saved.valid && st) {
        g_hid_saved.valid = true;
        g_hid_saved.enabled = enabled;
        g_hid_saved.transport = transport;
        g_hid_saved.types = types;
    }
    if (slot >= 0) {
        atomic_set_bit(g_hid_cfg_slots, slot);
        atomic_set_bit(g_hid_used_slots, slot);
    }
    k_spin_unlock(&g_hid_saved_lock, key);
}

/* Put the saved configuration back.  Runs on the system work queue: the
 * transport enable/disable path (USB/BLE) is too deep for the callers'
 * stacks and is not time-critical.  Only the steps that differ are taken:
 * a USB disable/enable cycle needlessly drops the host's view of the HID
 * interface. */
static void hid_restore_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    k_spinlock_key_t key = k_spin_lock(&g_hid_saved_lock);

    if (!g_hid_saved.valid || hid_cfg_any()) {
        /* nothing saved, or another app has changed it since: keep it */
        k_spin_unlock(&g_hid_saved_lock, key);
        return;
    }
    bool enabled = g_hid_saved.enabled;
    hid_transport_t transport = g_hid_saved.transport;
    hid_device_type_t types = g_hid_saved.types;

    g_hid_saved.valid = false;
    k_spin_unlock(&g_hid_saved_lock, key);

    const hid_state_t *st = hid_manager_get_state();
    if (!st || (st->enabled == enabled && st->transport == transport &&
                st->device_type == types)) {
        return;
    }

    LOG_INF("HID: restoring transport %d types 0x%02x (%s) after app exit",
            (int)transport, (unsigned)types, enabled ? "enabled" : "disabled");
    hid_manager_recover_locks();
    if (st->device_type != types) {
        /* types can only change while disabled */
        (void)hid_manager_disable();
        (void)hid_manager_set_device_types(types);
    }
    if (transport != HID_TRANSPORT_NONE && hid_manager_get_transport() != transport) {
        (void)hid_manager_set_transport(transport);
    }
    st = hid_manager_get_state();
    if (enabled && !st->enabled) {
        (void)hid_manager_enable();
    } else if (!enabled && st->enabled) {
        (void)hid_manager_disable();
    }
}
static K_WORK_DEFINE(g_hid_restore_work, hid_restore_work_fn);

/* Every hid_manager_* call takes the manager mutex (and may wait on the
 * transport): never let a stop abort the app thread inside one. */
#define HID_CALL(env, expr)                     \
    ({                                          \
        akira_native_critical_enter(env);       \
        int _hid_ret = (expr);                  \
        akira_native_critical_exit(env);        \
        _hid_ret;                               \
    })

/* strnlen() without relying on POSIX visibility (native_sim's host libc
 * hides it under -std=c99). */
static size_t bounded_strlen(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

/* ── App teardown ────────────────────────────────────────────────────────── */

/* Release what app @p slot left in HID.  Runs on the app thread, the stop
 * path or the system work queue; the app thread is finished or dead. */
static void hid_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);

    if (slot < 0 || slot >= AKIRA_MAX_WASM_INSTANCES) {
        return;
    }

    bool used = atomic_test_and_clear_bit(g_hid_used_slots, slot);
    bool changed_cfg = atomic_test_and_clear_bit(g_hid_cfg_slots, slot);

    if (!used && !changed_cfg) {
        return; /* never touched HID: send nothing */
    }

    /* An app that died inside the manager must not wedge everyone after it. */
    hid_manager_recover_locks();

    if (used) {
        /* Nothing the app pressed may stay held on the host. */
        (void)hid_keyboard_release_all();
        (void)hid_gamepad_reset();
        (void)hid_mouse_reset();
        (void)hid_consumer_send(0);
        hid_action_unregister_owner(slot);
        /* Reports queued for this app are not the next app's input. */
        k_msgq_purge(&hid_raw_msgq);
        k_msgq_purge(&hid_fido_msgq);
    }

    if (changed_cfg && !hid_cfg_any()) {
        /* Last app that reconfigured HID is gone: put the system's
         * transport/types back. */
        (void)k_work_submit(&g_hid_restore_work);
    }
}

AKIRA_APP_RESOURCE_DEFINE(hid, 50, NULL, hid_release);

#endif /* CONFIG_AKIRA_HID */

/* ── Device lifecycle ─────────────────────────────────────────────────────── */

int akira_native_hid_enable(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    hid_mark_config(exec_env);
    return HID_CALL(exec_env, hid_manager_enable());
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_disable(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    hid_mark_config(exec_env);
    return HID_CALL(exec_env, hid_manager_disable());
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_is_connected(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    return hid_manager_is_connected() ? 1 : 0;
#else
    return 0;
#endif
}

/* ── Keyboard ─────────────────────────────────────────────────────────────── */

int akira_native_hid_key_press(wasm_exec_env_t exec_env, int32_t keycode)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_HID_INJECT, NULL, -EACCES);

    if (keycode < 0 || keycode > 0xFF) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    hid_mark_used(exec_env);
    return HID_CALL(exec_env, hid_keyboard_press((hid_key_code_t)keycode));
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_key_release(wasm_exec_env_t exec_env, int32_t keycode)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (keycode < 0 || keycode > 0xFF) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    return HID_CALL(exec_env, hid_keyboard_release((hid_key_code_t)keycode));
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_set_modifiers(wasm_exec_env_t exec_env, int32_t mod_mask)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_HID_INJECT, NULL, -EACCES);

    if (mod_mask < 0 || mod_mask > 0xFF) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    hid_mark_used(exec_env);
    return HID_CALL(exec_env, hid_keyboard_set_modifiers((uint8_t)mod_mask));
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_key_release_all(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    return HID_CALL(exec_env, hid_keyboard_release_all());
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_type_string(wasm_exec_env_t exec_env, const char *str)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_HID_INJECT, NULL, -EACCES);

    if (!str) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    /* "$" validated the whole string.  Typed here, one key at a time, rather
     * than through hid_keyboard_type_string(): each locked step is a short
     * critical section, the 10 ms key timing is an interruptible sleep, and a
     * stop ends the string between keys with nothing left held.  ~20 ms per
     * character, so the length is capped. */
    size_t len = bounded_strlen(str, HID_TYPE_STRING_MAX + 1);
    if (len > HID_TYPE_STRING_MAX) {
        return -E2BIG;
    }
    hid_mark_used(exec_env);

    int ret = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t mod = 0;
        hid_key_code_t key = hid_ascii_to_keycode(str[i], &mod);

        if (key == HID_KEY_NONE) {
            continue;
        }
        if (akira_runtime_stop_requested(exec_env)) {
            ret = -EINTR;
            break;
        }

        akira_native_critical_enter(exec_env);
        if (mod) {
            (void)hid_keyboard_set_modifiers(mod);
        }
        int r = hid_keyboard_press(key);
        akira_native_critical_exit(exec_env);

        bool stopped = akira_runtime_sleep(exec_env, K_MSEC(10));

        akira_native_critical_enter(exec_env);
        (void)hid_keyboard_release(key);
        if (mod) {
            (void)hid_keyboard_set_modifiers(0);
        }
        akira_native_critical_exit(exec_env);

        if (r < 0) {
            ret = r; /* not initialised / no transport: give up */
            break;
        }
        if (stopped || akira_runtime_sleep(exec_env, K_MSEC(10))) {
            ret = -EINTR;
            break;
        }
    }
    if (ret == -EINTR) {
        /* Leave nothing held behind a stopped app. */
        (void)HID_CALL(exec_env, hid_keyboard_release_all());
    }
    return ret;
#else
    return -ENOTSUP;
#endif
}

/* ── Gamepad ──────────────────────────────────────────────────────────────── */

int akira_native_hid_gamepad_press(wasm_exec_env_t exec_env, int32_t btn_mask)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_HID_INJECT, NULL, -EACCES);

#ifdef CONFIG_AKIRA_HID
    hid_mark_used(exec_env);
    return HID_CALL(exec_env, hid_gamepad_press((hid_gamepad_btn_t)btn_mask));
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_gamepad_release(wasm_exec_env_t exec_env, int32_t btn_mask)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    return HID_CALL(exec_env, hid_gamepad_release((hid_gamepad_btn_t)btn_mask));
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_gamepad_set_axis(wasm_exec_env_t exec_env,
                                      int32_t axis, int32_t value)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_HID_INJECT, NULL, -EACCES);

    if (axis < 0 || axis >= HID_GAMEPAD_MAX_AXES) {
        return -EINVAL;
    }
    if (value < -32768 || value > 32767) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    hid_mark_used(exec_env);
    return HID_CALL(exec_env, hid_gamepad_set_axis((hid_gamepad_axis_t)axis, (int16_t)value));
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_gamepad_set_dpad(wasm_exec_env_t exec_env, int32_t direction)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_HID_INJECT, NULL, -EACCES);

    if (direction < 0 || direction > 8) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    hid_mark_used(exec_env);
    return HID_CALL(exec_env, hid_gamepad_set_dpad((uint8_t)direction));
#else
    return -ENOTSUP;
#endif
}

/*
 * Send all six axes + buttons + hat in a single BLE notify, instead of the
 * 5 separate notifies that pressing press/release/set_axis/set_dpad individually
 * would trigger — avoids BLE notification-queue congestion when several inputs
 * (IMU + D-pad) change in the same frame.
 */
int akira_native_hid_gamepad_send_report(wasm_exec_env_t exec_env,
                                         int32_t buttons, int32_t hat,
                                         int32_t a0, int32_t a1, int32_t a2,
                                         int32_t a3, int32_t a4, int32_t a5)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_HID_INJECT, NULL, -EACCES);

    if (hat < 0 || hat > 8 || buttons < 0 || buttons > 0xFFFF) {
        return -EINVAL;
    }

    const int32_t axes[] = { a0, a1, a2, a3, a4, a5 };

    for (size_t i = 0; i < ARRAY_SIZE(axes); i++) {
        if (axes[i] < -32768 || axes[i] > 32767) {
            return -EINVAL;
        }
    }

#ifdef CONFIG_AKIRA_HID
    hid_gamepad_report_t rpt = {
        .axes = { (int16_t)a0, (int16_t)a1, (int16_t)a2,
                  (int16_t)a3, (int16_t)a4, (int16_t)a5 },
        .buttons = (uint16_t)buttons,
        .hat = (uint8_t)hat,
    };

    /* Held buttons/axes outlive the call: teardown resets the gamepad. */
    hid_mark_used(exec_env);
    return HID_CALL(exec_env, hid_gamepad_send_report(&rpt));
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_gamepad_reset(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    return HID_CALL(exec_env, hid_gamepad_reset());
#else
    return -ENOTSUP;
#endif
}

/* ── Mouse ────────────────────────────────────────────────────────────────── */

int akira_native_hid_mouse_move(wasm_exec_env_t exec_env, int32_t dx, int32_t dy)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_HID_INJECT, NULL, -EACCES);

    /* Clamp to int8 range */
    int8_t cdx = (int8_t)CLAMP(dx, -127, 127);
    int8_t cdy = (int8_t)CLAMP(dy, -127, 127);

#ifdef CONFIG_AKIRA_HID
    hid_mark_used(exec_env);
    return HID_CALL(exec_env, hid_mouse_move(cdx, cdy));
#else
    ARG_UNUSED(cdx);
    ARG_UNUSED(cdy);
    return -ENOTSUP;
#endif
}

int akira_native_hid_mouse_btn_press(wasm_exec_env_t exec_env, int32_t button)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_HID_INJECT, NULL, -EACCES);

    if (button < 0 || button > 0xFF) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    hid_mark_used(exec_env);
    return HID_CALL(exec_env, hid_mouse_button_press((uint8_t)button));
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_mouse_btn_release(wasm_exec_env_t exec_env, int32_t button)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (button < 0 || button > 0xFF) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    return HID_CALL(exec_env, hid_mouse_button_release((uint8_t)button));
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_mouse_scroll(wasm_exec_env_t exec_env, int32_t delta)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_HID_INJECT, NULL, -EACCES);

    int8_t cd = (int8_t)CLAMP(delta, -127, 127);

#ifdef CONFIG_AKIRA_HID
    hid_mark_used(exec_env);
    return HID_CALL(exec_env, hid_mouse_scroll(cd));
#else
    ARG_UNUSED(cd);
    return -ENOTSUP;
#endif
}

/* ── Consumer / Media keys ────────────────────────────────────────────────── */

int akira_native_hid_consumer_send(wasm_exec_env_t exec_env, int32_t usage_code)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_HID_INJECT, NULL, -EACCES);

    if (usage_code < 0 || usage_code > 0xFFFF) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    hid_mark_used(exec_env);
    return HID_CALL(exec_env, hid_consumer_send((uint16_t)usage_code));
#else
    return -ENOTSUP;
#endif
}

/* ── Raw report ───────────────────────────────────────────────────────────── */

int akira_native_hid_send_raw_report(wasm_exec_env_t exec_env,
                                     int32_t report_id,
                                     uint32_t data_ptr, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_HID_INJECT, NULL, -EACCES);

    if (report_id < 0 || report_id > 0xFF || len == 0 || len > 64) {
        return -EINVAL;
    }

    /* Validate the whole [data_ptr, data_ptr+len) range, not just the base. */
    const uint8_t *ptr =
        (const uint8_t *)akira_wasm_ptr_to_native(exec_env, data_ptr, len);
    if (!ptr) {
        return -EFAULT;
    }

#ifdef CONFIG_AKIRA_HID
    uint8_t report[HID_RAW_REPORT_MAX];

    memcpy(report, ptr, len); /* out of app memory before taking any lock */
    hid_mark_used(exec_env);
    return HID_CALL(exec_env, hid_send_raw_report((uint8_t)report_id, report, len));
#else
    return -ENOTSUP;
#endif
}

/* ── Named action registry ────────────────────────────────────────────────── */

int akira_native_hid_action_register(wasm_exec_env_t exec_env,
                                     const char *name,
                                     int32_t modifier,
                                     int32_t keycode)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (!name || modifier < 0 || modifier > 0xFF ||
        keycode < 0 || keycode > 0xFF) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    int slot = hid_slot(exec_env);
    if (slot < 0) {
        return -EPERM;
    }
    hid_mark_used(exec_env);
    /* Owned by the calling slot: no other app can rebind it, and it is
     * dropped when the app goes. */
    return HID_CALL(exec_env, hid_action_register_owned(name, (uint8_t)modifier,
                                                        (uint8_t)keycode, slot));
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_action_trigger(wasm_exec_env_t exec_env, const char *name)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);
    AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_HID_INJECT, NULL, -EACCES);

    if (!name) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    int slot = hid_slot(exec_env);
    if (slot < 0) {
        return -EPERM;
    }
    hid_mark_used(exec_env);
    return HID_CALL(exec_env, hid_action_trigger_owned(name, slot));
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_set_transport(wasm_exec_env_t exec_env, int32_t transport)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (transport < 0 || transport > 3) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    hid_mark_config(exec_env);
    return HID_CALL(exec_env, hid_manager_set_transport((hid_transport_t)transport));
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_set_device_types(wasm_exec_env_t exec_env, int32_t types)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (types <= 0 || types > 0x07) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    hid_mark_config(exec_env);
    return HID_CALL(exec_env, hid_manager_set_device_types((hid_device_type_t)types));
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_init(wasm_exec_env_t exec_env, int32_t transport, int32_t types)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (transport < 0 || transport > 3) {
        LOG_WRN_RATELIMIT("hid_init: invalid transport %d", transport);
        return -EINVAL;
    }
    if (types <= 0 || types > 0x07) {
        LOG_WRN_RATELIMIT("hid_init: invalid device_types 0x%02x", types);
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    hid_mark_config(exec_env);
    int ret = HID_CALL(exec_env, hid_manager_setup((hid_transport_t)transport,
                                                   (hid_device_type_t)types));
    if (ret == 0 && transport == 2 /* HID_TRANSPORT_USB */)
    {
        akira_native_critical_enter(exec_env);
        hid_maybe_register_usb_handlers();
        akira_native_critical_exit(exec_env);
    }
    return ret;
#else
    return -ENOTSUP;
#endif
}

/* ── Raw / FIDO polled receive ────────────────────────────────────────────── */

int akira_native_hid_raw_recv(wasm_exec_env_t exec_env,
                               uint32_t buf_ptr, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    if (len < USB_HID_RAW_PAYLOAD_SIZE) {
        return -EINVAL;
    }

    uint8_t *ptr = (uint8_t *)akira_wasm_ptr_to_native(exec_env, buf_ptr, len);
    if (!ptr) {
        return -EFAULT;
    }

    struct hid_raw_pkt pkt;
    if (k_msgq_get(&hid_raw_msgq, &pkt, K_NO_WAIT) != 0) {
        return -EAGAIN;
    }

    hid_mark_used(exec_env); /* release purges what is still queued */
    memcpy(ptr, pkt.data, MIN(pkt.len, (uint8_t)USB_HID_RAW_PAYLOAD_SIZE));
    return (int)pkt.len;
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_fido_recv(wasm_exec_env_t exec_env,
                                uint32_t buf_ptr, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    if (len < USB_HID_FIDO_PAYLOAD_SIZE) {
        return -EINVAL;
    }

    uint8_t *ptr = (uint8_t *)akira_wasm_ptr_to_native(exec_env, buf_ptr, len);
    if (!ptr) {
        return -EFAULT;
    }

    struct hid_fido_pkt pkt;
    if (k_msgq_get(&hid_fido_msgq, &pkt, K_NO_WAIT) != 0) {
        return -EAGAIN;
    }

    hid_mark_used(exec_env);
    memcpy(ptr, pkt.data, MIN(pkt.len, (uint8_t)USB_HID_FIDO_PAYLOAD_SIZE));
    return (int)pkt.len;
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_fido_send(wasm_exec_env_t exec_env,
                                uint32_t buf_ptr, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    /* usb_hid_fido_send() lives in usb_hid.c, which CMake compiles only under
     * CONFIG_AKIRA_USB_HID. Gate on that (not CONFIG_AKIRA_HID) so a build with
     * HID enabled but USB HID disabled links cleanly, matching how the FIDO ISR
     * handler is registered above. */
#ifdef CONFIG_AKIRA_USB_HID
    if (len == 0 || len > USB_HID_FIDO_PAYLOAD_SIZE) {
        return -EINVAL;
    }

    /* Validate the whole [buf_ptr, buf_ptr+len) range, not just the base. */
    const uint8_t *ptr =
        (const uint8_t *)akira_wasm_ptr_to_native(exec_env, buf_ptr, len);
    if (!ptr) {
        return -EFAULT;
    }

    /* Pad to full FIDO payload size.  On the caller's stack, not a shared
     * static: two apps sending at once must not mix their reports
     * (usb_hid_fido_send() copies it before returning). */
    uint8_t __aligned(4) fido_buf[USB_HID_FIDO_PAYLOAD_SIZE];
    memset(fido_buf, 0, sizeof(fido_buf));
    memcpy(fido_buf, ptr, len);

    hid_mark_used(exec_env);
    /* Takes the USB report semaphore (<= 100 ms) that only the transfer
     * completion gives back: an abort in between would wedge USB HID. */
    return HID_CALL(exec_env, usb_hid_fido_send(fido_buf));
#else
    return -ENOTSUP;
#endif
}

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_HID))
#include <akira_native_registry.h>

static const NativeSymbol akira_hid_natives[] = {
    {"hid_enable", (void *)akira_native_hid_enable, "()i", NULL},
    {"hid_disable", (void *)akira_native_hid_disable, "()i", NULL},
    {"hid_is_connected", (void *)akira_native_hid_is_connected, "()i", NULL},
    {"hid_key_press",      (void *)akira_native_hid_key_press,      "(i)i",  NULL},
    {"hid_key_release",    (void *)akira_native_hid_key_release,    "(i)i",  NULL},
    {"hid_key_release_all",(void *)akira_native_hid_key_release_all,"()i",   NULL},
    {"hid_type_string",    (void *)akira_native_hid_type_string,    "($)i",  NULL},
    {"hid_set_modifiers",  (void *)akira_native_hid_set_modifiers,  "(i)i",  NULL},
    {"hid_gamepad_press", (void *)akira_native_hid_gamepad_press, "(i)i", NULL},
    {"hid_gamepad_release", (void *)akira_native_hid_gamepad_release, "(i)i", NULL},
    {"hid_gamepad_set_axis", (void *)akira_native_hid_gamepad_set_axis, "(ii)i", NULL},
    {"hid_gamepad_set_dpad", (void *)akira_native_hid_gamepad_set_dpad, "(i)i", NULL},
    {"hid_gamepad_send_report", (void *)akira_native_hid_gamepad_send_report, "(iiiiiiii)i", NULL},
    {"hid_gamepad_reset", (void *)akira_native_hid_gamepad_reset, "()i", NULL},
    {"hid_mouse_move", (void *)akira_native_hid_mouse_move, "(ii)i", NULL},
    {"hid_mouse_btn_press", (void *)akira_native_hid_mouse_btn_press, "(i)i", NULL},
    {"hid_mouse_btn_release", (void *)akira_native_hid_mouse_btn_release, "(i)i", NULL},
    {"hid_mouse_scroll", (void *)akira_native_hid_mouse_scroll, "(i)i", NULL},
    {"hid_consumer_send", (void *)akira_native_hid_consumer_send, "(i)i", NULL},
    {"hid_send_raw_report", (void *)akira_native_hid_send_raw_report, "(iii)i", NULL},
    {"hid_raw_recv",        (void *)akira_native_hid_raw_recv,        "(ii)i",  NULL},
    {"hid_fido_recv",       (void *)akira_native_hid_fido_recv,       "(ii)i",  NULL},
    {"hid_fido_send",       (void *)akira_native_hid_fido_send,       "(ii)i",  NULL},
    {"hid_action_register", (void *)akira_native_hid_action_register, "($ii)i", NULL},
    {"hid_action_trigger", (void *)akira_native_hid_action_trigger, "($)i", NULL},
    {"hid_set_transport", (void *)akira_native_hid_set_transport, "(i)i", NULL},
    {"hid_set_device_types", (void *)akira_native_hid_set_device_types, "(i)i", NULL},
    {"hid_init", (void *)akira_native_hid_init, "(ii)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_hid_api, "env", akira_hid_natives);
#endif
