/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_nfc
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_nfc, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_nfc_api.c
 * @brief NFC tag read/write/field-presence API for WASM applications.
 *
 * Chip-agnostic: goes through nfc_manager, which any registered NFC tag
 * driver (currently ST25DV) can serve. No mailbox/emulate support here —
 * this covers only static user-memory read/write and field detection.
 */

#include "akira_nfc_api.h"
#include <runtime/security.h>
#include <connectivity/nfc_interface.h>
#include <errno.h>

/* -------------------------------------------------------------------------- */
/* Core API (usable without WASM runtime)                                     */
/* -------------------------------------------------------------------------- */

int akira_nfc_uid(uint8_t uid[8])
{
    nfc_handle_t *h = nfc_manager_get(NFC_TYPE_NONE);
    if (!h) {
        return -ENODEV;
    }
    return nfc_read_uid(h, uid);
}

int akira_nfc_read(uint16_t addr, uint8_t *buf, size_t len)
{
    if (len == 0 || len > AKIRA_NFC_MAX_XFER_LEN) {
        return -EINVAL;
    }
    nfc_handle_t *h = nfc_manager_get(NFC_TYPE_NONE);
    if (!h) {
        return -ENODEV;
    }
    return nfc_read_mem(h, addr, buf, len);
}

int akira_nfc_write(uint16_t addr, const uint8_t *buf, size_t len)
{
    if (len == 0 || len > AKIRA_NFC_MAX_XFER_LEN) {
        return -EINVAL;
    }
    nfc_handle_t *h = nfc_manager_get(NFC_TYPE_NONE);
    if (!h) {
        return -ENODEV;
    }
    return nfc_write_mem(h, addr, buf, len);
}

int akira_nfc_field_present(void)
{
    nfc_handle_t *h = nfc_manager_get(NFC_TYPE_NONE);
    if (!h) {
        return -ENODEV;
    }
    bool present = false;
    int ret = nfc_field_present(h, &present);
    if (ret < 0) {
        return ret;
    }
    return present ? 1 : 0;
}

int akira_nfc_mb_enable(bool enable, uint8_t wdg)
{
    nfc_handle_t *h = nfc_manager_get(NFC_TYPE_NONE);
    if (!h) {
        return -ENODEV;
    }
    return nfc_mailbox_enable(h, enable, wdg);
}

int akira_nfc_mb_put(const uint8_t *buf, size_t len)
{
    if (len == 0 || len > AKIRA_NFC_MAX_XFER_LEN) {
        return -EINVAL;
    }
    nfc_handle_t *h = nfc_manager_get(NFC_TYPE_NONE);
    if (!h) {
        return -ENODEV;
    }
    return nfc_mailbox_put_msg(h, buf, len);
}

int akira_nfc_mb_get(uint8_t *buf, size_t cap)
{
    if (cap == 0 || cap > AKIRA_NFC_MAX_XFER_LEN) {
        return -EINVAL;
    }
    nfc_handle_t *h = nfc_manager_get(NFC_TYPE_NONE);
    if (!h) {
        return -ENODEV;
    }
    size_t len_out = 0;
    int ret = nfc_mailbox_get_msg(h, buf, cap, &len_out);
    if (ret < 0) {
        return ret;
    }
    return (int)len_out;
}

int akira_nfc_mb_status(uint8_t *ctrl_out, size_t *msg_len_out)
{
    nfc_handle_t *h = nfc_manager_get(NFC_TYPE_NONE);
    if (!h) {
        return -ENODEV;
    }
    return nfc_mailbox_status(h, ctrl_out, msg_len_out);
}

/* -------------------------------------------------------------------------- */
/* WASM native exports                                                         */
/* -------------------------------------------------------------------------- */

#ifdef CONFIG_AKIRA_WASM_RUNTIME

int akira_native_nfc_uid(wasm_exec_env_t exec_env, uint8_t *uid_out)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    if (!uid_out) {
        return -EINVAL;
    }
    return akira_nfc_uid(uid_out);
}

int akira_native_nfc_read(wasm_exec_env_t exec_env, int32_t addr, uint8_t *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    if (!buf || (uint32_t)addr > 0xFFFF) {
        return -EINVAL;
    }
    return akira_nfc_read((uint16_t)addr, buf, len);
}

int akira_native_nfc_write(wasm_exec_env_t exec_env, int32_t addr, const uint8_t *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    if (!buf || (uint32_t)addr > 0xFFFF) {
        return -EINVAL;
    }
    return akira_nfc_write((uint16_t)addr, buf, len);
}

int akira_native_nfc_field_present(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    return akira_nfc_field_present();
}

int akira_native_nfc_mb_enable(wasm_exec_env_t exec_env, int32_t enable, uint8_t wdg)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    return akira_nfc_mb_enable(enable != 0, wdg);
}

int akira_native_nfc_mb_put(wasm_exec_env_t exec_env, uint8_t *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    if (!buf) {
        return -EINVAL;
    }
    return akira_nfc_mb_put(buf, len);
}

int akira_native_nfc_mb_get(wasm_exec_env_t exec_env, uint8_t *buf, uint32_t cap)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    if (!buf) {
        return -EINVAL;
    }
    return akira_nfc_mb_get(buf, cap);
}

int akira_native_nfc_mb_status(wasm_exec_env_t exec_env, uint8_t *ctrl_out, uint32_t *msg_len_out)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    if (!ctrl_out || !msg_len_out) {
        return -EINVAL;
    }
    size_t msg_len = 0;
    int ret = akira_nfc_mb_status(ctrl_out, &msg_len);
    if (ret == 0) {
        *msg_len_out = (uint32_t)msg_len;
    }
    return ret;
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
