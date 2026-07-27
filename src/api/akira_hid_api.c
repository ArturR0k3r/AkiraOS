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

struct hid_raw_pkt  { uint8_t data[USB_HID_RAW_PAYLOAD_SIZE];  uint8_t len; };
struct hid_fido_pkt { uint8_t data[USB_HID_FIDO_PAYLOAD_SIZE]; uint8_t len; };

K_MSGQ_DEFINE(hid_raw_msgq,  sizeof(struct hid_raw_pkt),  HID_RAW_QUEUE_DEPTH,  4);
K_MSGQ_DEFINE(hid_fido_msgq, sizeof(struct hid_fido_pkt), HID_FIDO_QUEUE_DEPTH, 4);

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

static bool hid_usb_handlers_registered = false;

static void hid_maybe_register_usb_handlers(void)
{
#ifdef CONFIG_AKIRA_USB_HID
    if (!hid_usb_handlers_registered)
    {
        usb_hid_raw_set_handler(hid_raw_isr_cb);
        usb_hid_fido_set_handler(hid_fido_isr_cb);
        hid_usb_handlers_registered = true;
    }
#endif
}

#endif /* CONFIG_AKIRA_HID */

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

int akira_native_hid_set_modifiers(wasm_exec_env_t exec_env, int32_t mod_mask)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_HID, -EPERM);

    if (mod_mask < 0 || mod_mask > 0xFF) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    return hid_keyboard_set_modifiers((uint8_t)mod_mask);
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

    if (hat < 0 || hat > 8) {
        return -EINVAL;
    }

#ifdef CONFIG_AKIRA_HID
    hid_gamepad_report_t rpt = {
        .axes = { (int16_t)a0, (int16_t)a1, (int16_t)a2,
                  (int16_t)a3, (int16_t)a4, (int16_t)a5 },
        .buttons = (uint16_t)buttons,
        .hat = (uint8_t)hat,
    };
    return hid_gamepad_send_report(&rpt);
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
    int ret = hid_manager_setup((hid_transport_t)transport, (hid_device_type_t)types);
    if (ret == 0 && transport == 2 /* HID_TRANSPORT_USB */)
    {
        hid_maybe_register_usb_handlers();
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

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return -EINVAL;
    }

    uint8_t *ptr = (uint8_t *)wasm_runtime_addr_app_to_native(module_inst, buf_ptr);
    if (!ptr) {
        return -EFAULT;
    }

    struct hid_raw_pkt pkt;
    if (k_msgq_get(&hid_raw_msgq, &pkt, K_NO_WAIT) != 0) {
        return -EAGAIN;
    }

    memcpy(ptr, pkt.data, pkt.len);
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

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return -EINVAL;
    }

    uint8_t *ptr = (uint8_t *)wasm_runtime_addr_app_to_native(module_inst, buf_ptr);
    if (!ptr) {
        return -EFAULT;
    }

    struct hid_fido_pkt pkt;
    if (k_msgq_get(&hid_fido_msgq, &pkt, K_NO_WAIT) != 0) {
        return -EAGAIN;
    }

    memcpy(ptr, pkt.data, pkt.len);
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

    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst) {
        return -EINVAL;
    }

    const uint8_t *ptr =
        (const uint8_t *)wasm_runtime_addr_app_to_native(module_inst, buf_ptr);
    if (!ptr) {
        return -EFAULT;
    }

    /* Pad to full FIDO payload size */
    static uint8_t __aligned(4) fido_buf[USB_HID_FIDO_PAYLOAD_SIZE];
    memset(fido_buf, 0, sizeof(fido_buf));
    memcpy(fido_buf, ptr, len);

    return usb_hid_fido_send(fido_buf);
#else
    return -ENOTSUP;
#endif
}
