/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_hid_api
#include <zephyr/logging/log.h>
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
 */

#include "akira_hid_api.h"
#include <runtime/security.h>
#include <zephyr/kernel.h>
#include <string.h>

#ifdef CONFIG_AKIRA_HID
#include <connectivity/hid/hid_manager.h>
#endif

/* ── Device lifecycle ─────────────────────────────────────────────────────── */

int akira_native_hid_enable(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    return hid_manager_enable();
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_disable(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    return hid_manager_disable();
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

    if (keycode < 0 || keycode > 0xFF) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    return hid_keyboard_press((hid_key_code_t)keycode);
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
    return hid_keyboard_release((hid_key_code_t)keycode);
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_key_release_all(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    return hid_keyboard_release_all();
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_type_string(wasm_exec_env_t exec_env, const char *str)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (!str) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    return hid_keyboard_type_string(str);
#else
    return -ENOTSUP;
#endif
}

/* ── Gamepad ──────────────────────────────────────────────────────────────── */

int akira_native_hid_gamepad_press(wasm_exec_env_t exec_env, int32_t btn_mask)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    return hid_gamepad_press((hid_gamepad_btn_t)btn_mask);
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_gamepad_release(wasm_exec_env_t exec_env, int32_t btn_mask)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    return hid_gamepad_release((hid_gamepad_btn_t)btn_mask);
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_gamepad_set_axis(wasm_exec_env_t exec_env,
                                      int32_t axis, int32_t value)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (axis < 0 || axis >= HID_GAMEPAD_MAX_AXES) {
        return -EINVAL;
    }
    if (value < -32768 || value > 32767) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    return hid_gamepad_set_axis((hid_gamepad_axis_t)axis, (int16_t)value);
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_gamepad_set_dpad(wasm_exec_env_t exec_env, int32_t direction)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (direction < 0 || direction > 8) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    return hid_gamepad_set_dpad((uint8_t)direction);
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_gamepad_reset(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

#ifdef CONFIG_AKIRA_HID
    return hid_gamepad_reset();
#else
    return -ENOTSUP;
#endif
}

/* ── Mouse ────────────────────────────────────────────────────────────────── */

int akira_native_hid_mouse_move(wasm_exec_env_t exec_env, int32_t dx, int32_t dy)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    /* Clamp to int8 range */
    int8_t cdx = (int8_t)CLAMP(dx, -127, 127);
    int8_t cdy = (int8_t)CLAMP(dy, -127, 127);

#ifdef CONFIG_AKIRA_HID
    return hid_mouse_move(cdx, cdy);
#else
    ARG_UNUSED(cdx);
    ARG_UNUSED(cdy);
    return -ENOTSUP;
#endif
}

int akira_native_hid_mouse_btn_press(wasm_exec_env_t exec_env, int32_t button)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (button < 0 || button > 0xFF) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    return hid_mouse_button_press((uint8_t)button);
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
    return hid_mouse_button_release((uint8_t)button);
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_mouse_scroll(wasm_exec_env_t exec_env, int32_t delta)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    int8_t cd = (int8_t)CLAMP(delta, -127, 127);

#ifdef CONFIG_AKIRA_HID
    return hid_mouse_scroll(cd);
#else
    ARG_UNUSED(cd);
    return -ENOTSUP;
#endif
}

/* ── Consumer / Media keys ────────────────────────────────────────────────── */

int akira_native_hid_consumer_send(wasm_exec_env_t exec_env, int32_t usage_code)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (usage_code < 0 || usage_code > 0xFFFF) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    return hid_consumer_send((uint16_t)usage_code);
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

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return -EINVAL;
    }

    if (len == 0 || len > 64) {
        return -EINVAL;
    }

    const uint8_t *ptr =
        (const uint8_t *)wasm_runtime_addr_app_to_native(module_inst, data_ptr);
    if (!ptr) {
        return -EFAULT;
    }

#ifdef CONFIG_AKIRA_HID
    return hid_send_raw_report((uint8_t)report_id, ptr, len);
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
    return hid_action_register(name, (uint8_t)modifier, (uint8_t)keycode);
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_action_trigger(wasm_exec_env_t exec_env, const char *name)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (!name) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    return hid_action_trigger(name);
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
    return hid_manager_set_transport((hid_transport_t)transport);
#else
    return -ENOTSUP;
#endif
}

int akira_native_hid_set_device_types(wasm_exec_env_t exec_env, int32_t types)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (types <= 0) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    return hid_manager_set_device_types((hid_device_type_t)types);
#endif
}

int akira_native_hid_init(wasm_exec_env_t exec_env, int32_t transport, int32_t types)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (transport < 0 || transport > 3) {
        LOG_ERR("hid_init: invalid transport %d", transport);
        return -EINVAL;
    }
    if (types <= 0 || types > 0x07) {
        LOG_ERR("hid_init: invalid device_types 0x%02x", types);
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    return hid_manager_setup((hid_transport_t)transport, (hid_device_type_t)types);
#else
    return -ENOTSUP;
#endif
}
