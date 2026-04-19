/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_hid_api.h
 * @brief HID WASM API — keyboard, gamepad, mouse, consumer, raw report, named actions
 *
 * All exported functions require AKIRA_CAP_HID in the app manifest.
 *
 * Manifest entry:  "capabilities": ["hid"]
 */

#ifndef AKIRA_HID_API_H
#define AKIRA_HID_API_H

#include <stdint.h>
#include <stddef.h>
#include <wasm_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Device lifecycle ─────────────────────────────────────────────────────── */
int akira_native_hid_enable(wasm_exec_env_t exec_env);
int akira_native_hid_disable(wasm_exec_env_t exec_env);
int akira_native_hid_is_connected(wasm_exec_env_t exec_env);

/* ── Keyboard ─────────────────────────────────────────────────────────────── */
int akira_native_hid_key_press(wasm_exec_env_t exec_env, int32_t keycode);
int akira_native_hid_key_release(wasm_exec_env_t exec_env, int32_t keycode);
int akira_native_hid_key_release_all(wasm_exec_env_t exec_env);
int akira_native_hid_type_string(wasm_exec_env_t exec_env, const char *str);

/* ── Gamepad ──────────────────────────────────────────────────────────────── */
int akira_native_hid_gamepad_press(wasm_exec_env_t exec_env, int32_t btn_mask);
int akira_native_hid_gamepad_release(wasm_exec_env_t exec_env, int32_t btn_mask);
int akira_native_hid_gamepad_set_axis(wasm_exec_env_t exec_env, int32_t axis, int32_t value);
int akira_native_hid_gamepad_set_dpad(wasm_exec_env_t exec_env, int32_t direction);
int akira_native_hid_gamepad_reset(wasm_exec_env_t exec_env);

/* ── Mouse ────────────────────────────────────────────────────────────────── */
int akira_native_hid_mouse_move(wasm_exec_env_t exec_env, int32_t dx, int32_t dy);
int akira_native_hid_mouse_btn_press(wasm_exec_env_t exec_env, int32_t button);
int akira_native_hid_mouse_btn_release(wasm_exec_env_t exec_env, int32_t button);
int akira_native_hid_mouse_scroll(wasm_exec_env_t exec_env, int32_t delta);

/* ── Consumer / Media keys ────────────────────────────────────────────────── */
int akira_native_hid_consumer_send(wasm_exec_env_t exec_env, int32_t usage_code);

/* ── Raw report ───────────────────────────────────────────────────────────── */
int akira_native_hid_send_raw_report(wasm_exec_env_t exec_env,
                                     int32_t report_id,
                                     uint32_t data_ptr, uint32_t len);

/* ── Named action registry ────────────────────────────────────────────────── */
int akira_native_hid_action_register(wasm_exec_env_t exec_env,
                                     const char *name,
                                     int32_t modifier,
                                     int32_t keycode);
int akira_native_hid_action_trigger(wasm_exec_env_t exec_env, const char *name);

/* ── Transport ────────────────────────────────────────────────────────────── */
/** @brief Select HID transport (0=none, 1=BLE, 2=USB). Requires HID cap. */
int akira_native_hid_set_transport(wasm_exec_env_t exec_env, int32_t transport);

/** @brief Set device type bitmask before enabling HID (KEYBOARD=1, GAMEPAD=2,
 *         MOUSE=4). Returns -EBUSY if HID is already enabled. Requires HID cap. */
int akira_native_hid_set_device_types(wasm_exec_env_t exec_env, int32_t types);

/** @brief One-shot setup: set transport + device types + enable.
 *         Equivalent to calling hid_set_transport, hid_set_device_types,
 *         and hid_enable in sequence. Requires HID cap. */
int akira_native_hid_init(wasm_exec_env_t exec_env, int32_t transport, int32_t types);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_HID_API_H */
