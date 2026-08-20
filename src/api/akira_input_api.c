/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_input
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_input, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_input_api.c
 * @brief Button input native API — wraps Zephyr gpio-keys via input subsystem.
 *
 * One INPUT_CALLBACK_DEFINE registered at link time handles all gpio-keys events
 * on AkiraConsole (only one input device exists on this board).
 *
 * Consumers (native C or WASM):
 *   akira_input_get_bitmask()    — atomic snapshot of held buttons
 *   akira_input_poll_event()     — edge event dequeue (non-blocking)
 */

#include "akira_input_api.h"

#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <errno.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#include <runtime/security.h>
#endif

/* ── State ───────────────────────────────────────────────────────────────── */

/* Atomic bitmask: bit N = (1 << zephyr,code) for button N.  Updated in ISR
 * context via atomic_or/and — safe to read from any thread without locking. */
static atomic_t g_btn_state;

/* Latest absolute dial position (INPUT_ABS_WHEEL, 0–255). */
static atomic_t g_dial_value;

/* Ring buffer: up to 16 unprocessed edge events (press + release pairs). */
#define EVT_QUEUE_LEN 16
K_MSGQ_DEFINE(g_event_queue, sizeof(akira_input_event_t), EVT_QUEUE_LEN, 4);

/* ── Zephyr input callback ───────────────────────────────────────────────── */

/*
 * Called by the Zephyr input subsystem whenever a gpio-keys state change is
 * detected.  The callback runs on the input thread (CONFIG_INPUT_MODE_THREAD,
 * the default) or directly from the GPIO ISR (CONFIG_INPUT_MODE_SYNCHRONOUS).
 *
 * Both modes are safe here:
 *   - atomic_or/and are ISR-safe.
 *   - k_msgq_put with K_NO_WAIT is ISR-safe (non-blocking, drops on overflow).
 *
 * evt->code  = zephyr,code from DTS (2–9 for AkiraConsole buttons)
 * evt->value = 1 (pressed) or 0 (released)
 * evt->type  = INPUT_EV_KEY (we ignore EV_REL/EV_ABS)
 */
static void akira_input_cb(struct input_event *evt, void *user_data)
{
    ARG_UNUSED(user_data);

    if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_WHEEL) {
        /* Dial position update — store atomically (0–255). */
        atomic_set(&g_dial_value, (atomic_val_t)(evt->value & 0xFF));
        return;
    }

    if (evt->type != INPUT_EV_KEY) {
        return;
    }

    /* Update held-state bitmask. */
    uint32_t bit = (1U << evt->code);
    if (evt->value) {
        atomic_or(&g_btn_state, (atomic_val_t)bit);
    } else {
        atomic_and(&g_btn_state, (atomic_val_t)~bit);
    }

    /* Enqueue the edge event; silently drop if ring buffer is full.
     * A full queue means the WASM app isn't draining fast enough — not fatal. */
    akira_input_event_t e = {
        .button_id = (uint32_t)evt->code,
        .pressed   = (uint32_t)evt->value,
    };
    (void)k_msgq_put(&g_event_queue, &e, K_NO_WAIT);
}

/* Register callback at link time.  NULL = match any input device.
 * On AkiraConsole there is exactly one input device (the gpio-keys node),
 * so NULL is correct and avoids a DT reference to the specific node. */
INPUT_CALLBACK_DEFINE(NULL, akira_input_cb, NULL);

/* ── Native API ──────────────────────────────────────────────────────────── */

void akira_input_init(void)
{
    atomic_set(&g_btn_state, 0);
    atomic_set(&g_dial_value, 0);
    k_msgq_purge(&g_event_queue);
    LOG_INF("Input API initialized (queue depth=%d)", EVT_QUEUE_LEN);
}

uint32_t akira_input_get_bitmask(void)
{
    return (uint32_t)atomic_get(&g_btn_state);
}

int akira_input_poll_event(akira_input_event_t *evt_out)
{
    if (!evt_out) {
        return -EINVAL;
    }
    /* k_msgq_get returns 0 on success, -EAGAIN (or -ENOMSG) if empty. */
    return k_msgq_get(&g_event_queue, evt_out, K_NO_WAIT);
}

int akira_input_get_dial(void)
{
    return (int)(uint8_t)atomic_get(&g_dial_value);
}

/* ── WASM native exports ─────────────────────────────────────────────────── */
#ifdef CONFIG_AKIRA_WASM_RUNTIME

/*
 * input_get_buttons() → int  (WASM signature: "()i")
 *
 * Returns the current button bitmask as a signed int32 (WASM has no uint32).
 * Bit N is set when the button with zephyr,code == N is currently held.
 * Use the AKIRA_BTN_* macros from akira_console.h to test individual buttons.
 *
 * Capability: "input.read" (AKIRA_CAP_INPUT_READ)
 */
int akira_native_input_get_buttons(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_INPUT_READ, -EPERM);
    return (int)akira_input_get_bitmask();
}

/*
 * input_poll_event(buf_ptr, buf_len) → int  (WASM signature: "(*~)i")
 *
 * Drains one edge event from the ring buffer into the caller's WASM buffer.
 * Returns:
 *   1  — event dequeued and written to *buf_ptr
 *   0  — queue empty, no event
 *  <0  — error (-EINVAL: bad pointer or buf too small, -EPERM: no capability)
 *
 * The caller's buffer must be at least sizeof(akira_input_event_t) = 8 bytes.
 * Capability: "input.read" (AKIRA_CAP_INPUT_READ)
 */
int akira_native_input_poll_event(wasm_exec_env_t exec_env,
                                  uint32_t evt_ptr, uint32_t evt_len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_INPUT_READ, -EPERM);

    if (evt_len < (uint32_t)sizeof(akira_input_event_t)) {
        LOG_ERR("input_poll_event: buffer too small (%u < %zu)",
                evt_len, sizeof(akira_input_event_t));
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst) {
        return -EINVAL;
    }

    /* "(*~)" ABI: WAMR already converted the app address to a native
     * pointer before invoking us — do NOT call addr_app_to_native again
     * (double conversion yields a garbage host address).  Mirror
     * akira_native_wifi_scan_aps, which uses the passed pointer directly. */
    void *native_ptr = (void *)evt_ptr;
    if (!native_ptr || !wasm_runtime_validate_native_addr(inst, native_ptr,
                                                          sizeof(akira_input_event_t))) {
        LOG_ERR("input_poll_event: invalid WASM pointer 0x%08x", evt_ptr);
        return -EINVAL;
    }

    akira_input_event_t e;
    int ret = akira_input_poll_event(&e);
    if (ret == 0) {
        /* Write to WASM linear memory through the validated native pointer. */
        akira_input_event_t *dst = (akira_input_event_t *)native_ptr;
        dst->button_id = e.button_id;
        dst->pressed   = e.pressed;
        return 1;
    }
    /* -EAGAIN from k_msgq_get → queue empty → return 0 to caller. */
    return 0;
}

/*
 * input_get_dial() → int  (WASM signature: "()i")
 *
 * Returns the latest dial (rotary encoder) position as an integer in
 * the range 0–255.  The value is updated every ~50 ms by the PWM-dial
 * driver and is always safe to poll without draining a queue.
 * Capability: "input.read" (AKIRA_CAP_INPUT_READ)
 */
int akira_native_input_get_dial(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_INPUT_READ, -EPERM);
    return akira_input_get_dial();
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
