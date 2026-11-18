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

#include <akira_app_containment.h>
#include <string.h>

#define NFC_UID_LEN 8

/* '*' in the registration only validated the first byte: check the full
 * span the native will write. */
static bool nfc_app_span_ok(wasm_exec_env_t exec_env, void *ptr, uint32_t len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);

    return inst && ptr && wasm_runtime_validate_native_addr(inst, ptr, (uint64_t)len);
}

/* Every call below runs a transaction on the tag / secure element (I2C,
 * driver lock held, bounded by AKIRA_NFC_MAX_XFER_LEN and the driver's
 * timeouts).  On AkiraConsole Prod that is the SE050 that also holds the
 * Seal key: aborting the app mid-transaction would leak the driver lock and
 * desync the T=1 session, so the work runs as a native critical section. */
#define NFC_CRITICAL(env, expr)                 \
    ({                                          \
        akira_native_critical_enter(env);       \
        int nfc_crit_ret_ = (expr);             \
        akira_native_critical_exit(env);        \
        nfc_crit_ret_;                          \
    })

/* Registered "(*)i": validate all NFC_UID_LEN bytes the driver writes. */
int akira_native_nfc_uid(wasm_exec_env_t exec_env, uint8_t *uid_out)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    if (!uid_out) {
        return -EINVAL;
    }
    if (!nfc_app_span_ok(exec_env, uid_out, NFC_UID_LEN)) {
        return -EFAULT;
    }
    return NFC_CRITICAL(exec_env, akira_nfc_uid(uid_out));
}

int akira_native_nfc_read(wasm_exec_env_t exec_env, int32_t addr, uint8_t *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    if (!buf || (uint32_t)addr > 0xFFFF) {
        return -EINVAL;
    }
    return NFC_CRITICAL(exec_env, akira_nfc_read((uint16_t)addr, buf, len));
}

int akira_native_nfc_write(wasm_exec_env_t exec_env, int32_t addr, const uint8_t *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    if (!buf || (uint32_t)addr > 0xFFFF) {
        return -EINVAL;
    }
    return NFC_CRITICAL(exec_env, akira_nfc_write((uint16_t)addr, buf, len));
}

int akira_native_nfc_field_present(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    return NFC_CRITICAL(exec_env, akira_nfc_field_present());
}

int akira_native_nfc_mb_enable(wasm_exec_env_t exec_env, int32_t enable, uint8_t wdg)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    return NFC_CRITICAL(exec_env, akira_nfc_mb_enable(enable != 0, wdg));
}

int akira_native_nfc_mb_put(wasm_exec_env_t exec_env, uint8_t *buf, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    if (!buf) {
        return -EINVAL;
    }
    return NFC_CRITICAL(exec_env, akira_nfc_mb_put(buf, len));
}

int akira_native_nfc_mb_get(wasm_exec_env_t exec_env, uint8_t *buf, uint32_t cap)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    if (!buf) {
        return -EINVAL;
    }
    return NFC_CRITICAL(exec_env, akira_nfc_mb_get(buf, cap));
}

/* Registered "(**)i": ctrl_out is 1 byte (covered by '*'), msg_len_out is a
 * 4-byte store — validate it, and write it with memcpy: an app pointer need
 * not be 4-byte aligned, and an unaligned 32-bit store faults on Xtensa. */
int akira_native_nfc_mb_status(wasm_exec_env_t exec_env, uint8_t *ctrl_out, uint32_t *msg_len_out)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_NFC, -EPERM);
    if (!ctrl_out || !msg_len_out) {
        return -EINVAL;
    }
    if (!nfc_app_span_ok(exec_env, ctrl_out, 1) ||
        !nfc_app_span_ok(exec_env, msg_len_out, sizeof(uint32_t))) {
        return -EFAULT;
    }
    uint8_t ctrl = 0;
    size_t msg_len = 0;
    int ret = NFC_CRITICAL(exec_env, akira_nfc_mb_status(&ctrl, &msg_len));
    if (ret == 0) {
        uint32_t len32 = (uint32_t)msg_len;
        *ctrl_out = ctrl;
        memcpy(msg_len_out, &len32, sizeof(len32));
    }
    return ret;
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */
