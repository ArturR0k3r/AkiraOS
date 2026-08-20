/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * @file akira_input_api.h
 * @brief AkiraConsole button input native API.
 *
 * Wraps Zephyr's input subsystem (gpio-keys driver) into two WASM-callable
 * functions:
 *   input_get_buttons()  — atomic bitmask of currently held buttons
 *   input_poll_event()   — drain one press/release edge from a ring buffer
 *
 * Capability required: "input.read" (AKIRA_CAP_INPUT_READ)
 */

#ifndef AKIRA_INPUT_API_H
#define AKIRA_INPUT_API_H

#include <stdint.h>
#include <errno.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

/**
 * @brief Button edge event written into WASM linear memory by input_poll_event().
 *
 * Layout is identical on both the native (C struct) and WASM sides — 8 bytes,
 * naturally aligned.  Mirror this typedef in the WASM SDK akira_api.h.
 */
typedef struct
{
    uint32_t button_id; /**< AKIRA_BTN_ID_* constant (== zephyr,code value) */
    uint32_t pressed;   /**< 1 = key pressed, 0 = key released              */
} akira_input_event_t;

/**
 * @brief Button ID constants — match the zephyr,code values in the DTS overlay.
 * Bit N of akira_input_get_bitmask() is set when the button with code N is held.
 *
 * Physical wiring note: on akiraconsole_prod, X is on GPIO16 (SW3) and B is on
 * GPIO17 (SW4); node names follow silkscreen.  Codes below are the logical
 * constants apps use and match akira_console.h (B=8, X=9, Y=7).
 */
#define AKIRA_BTN_HOME 1 /**< Home/OK button (GPIO0, active-low pull-up) */
#define AKIRA_BTN_UP 2
#define AKIRA_BTN_DOWN 3
#define AKIRA_BTN_LEFT 4
#define AKIRA_BTN_RIGHT 5
#define AKIRA_BTN_A 6 /**< Confirm / launch */
#define AKIRA_BTN_B 8 /**< Back / cancel */
#define AKIRA_BTN_X 9
#define AKIRA_BTN_Y 7 /**< Context menu / alternate action */

/**
 * @brief Maximum dial axis value returned by akira_input_get_dial().
 * The dial reports 0 (fully counter-clockwise) to 255 (fully clockwise).
 */
#define AKIRA_DIAL_MAX 255

/* ── Native (non-WASM) API ───────────────────────────────────────────────── */

#ifdef CONFIG_AKIRA_INPUT_API
/**
 * @brief Initialise the input subsystem adapter.
 *
 * Clears the atomic button state and drains any stale events from the queue.
 * The INPUT_CALLBACK_DEFINE handler is registered at link time and requires no
 * runtime call — this function is idempotent and safe to call more than once.
 */
void akira_input_init(void);

/**
 * @brief Return the current button state as a bitmask.
 * Bit N = (1 << zephyr,code).  Updated atomically in the input callback.
 * @return Bitmask of all currently held buttons.
 */
uint32_t akira_input_get_bitmask(void);

/**
 * @brief Drain one press/release event from the ring buffer (non-blocking).
 * @param evt_out  Output buffer for the event (must not be NULL).
 * @return 0 on success (event filled), -EAGAIN if the queue is empty.
 */

int akira_input_poll_event(akira_input_event_t *evt_out);

/**
 * @brief Return the current dial (rotary encoder) position.
 *
 * The akira,pwm-dial driver fires INPUT_ABS_WHEEL axis events which are
 * captured here and stored atomically.  The returned value is 0 when the
 * knob is at the counter-clockwise stop and AKIRA_DIAL_MAX (255) at the
 * clockwise stop.  Returns 0 if no dial hardware is present.
 */
int akira_input_get_dial(void);

#else /* !CONFIG_AKIRA_INPUT_API — stubs for targets without gpio-keys (e.g. native_sim) */

static inline void akira_input_init(void) {}
static inline uint32_t akira_input_get_bitmask(void) { return 0U; }
static inline int akira_input_poll_event(akira_input_event_t *evt_out)
{
    (void)evt_out;
    return -EAGAIN;
}
static inline int akira_input_get_dial(void) { return 0; }

#endif /* CONFIG_AKIRA_INPUT_API */

/* ── WASM native exports ─────────────────────────────────────────────────── */
#ifdef CONFIG_AKIRA_WASM_RUNTIME

/**
 * WASM signature: ()i
 * Returns the button bitmask cast to int32 (WASM has no uint32 type).
 * Requires capability: AKIRA_CAP_INPUT_READ ("input.read")
 */
int akira_native_input_get_buttons(wasm_exec_env_t exec_env);

/**
 * WASM signature: (*~)i  (buf_ptr, buf_len) → int
 * Fills the caller's 8-byte akira_input_event_t buffer.
 * Returns 1 if an event was available, 0 if none, <0 on error.
 * Requires capability: AKIRA_CAP_INPUT_READ ("input.read")
 */
int akira_native_input_poll_event(wasm_exec_env_t exec_env,
                                  uint32_t evt_ptr, uint32_t evt_len);

/**
 * WASM signature: ()i
 * Returns the current dial position (0–255).
 * Requires capability: AKIRA_CAP_INPUT_READ ("input.read")
 */
int akira_native_input_get_dial(wasm_exec_env_t exec_env);

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#endif /* AKIRA_INPUT_API_H */
