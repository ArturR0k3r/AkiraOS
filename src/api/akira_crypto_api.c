/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_crypto_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_crypto_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_crypto_api.c
 * @brief Cryptographic operations WASM native API.
 *
 * Backend: Zephyr TinyCrypt (CONFIG_TINYCRYPT=y).
 * - Key buffers are zeroed after use (prevent leaking key material).
 * - All input lengths capped at CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT (default 64KB).
 * - Every operation validates WASM linear memory bounds before use.
 *
 * Gate: CONFIG_AKIRA_WASM_CRYPTO=y
 * Capability: AKIRA_CAP_CRYPTO (bit 28)
 */

#ifdef CONFIG_AKIRA_WASM_CRYPTO

#include "akira_crypto_api.h"
#include <runtime/security.h>
#include <runtime/akira_runtime.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

#include <tinycrypt/sha256.h>
#include <tinycrypt/aes.h>
#include <tinycrypt/cbc_mode.h>
#include <tinycrypt/hmac.h>
#include <tinycrypt/constants.h>
#include <zephyr/random/random.h>

#ifndef CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT
#define CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT (64u * 1024u)
#endif

/* ── helpers ────────────────────────────────────────────────────────────── */

/** Validate a WASM pointer/length and abort on failure. */
#define WASM_ADDR_CHECK(inst, ptr, len)                                 \
    do {                                                                 \
        if (!(ptr) || !wasm_runtime_validate_native_addr((inst),        \
                                                          (ptr), (len)))\
        { return -EFAULT; }                                              \
    } while (0)

/* ── sha256 ─────────────────────────────────────────────────────────────── */

int akira_native_crypto_sha256(wasm_exec_env_t exec_env,
                                void *data_ptr, uint32_t data_len,
                                void *out_ptr)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

    if (data_len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
        return -EMSGSIZE;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, data_ptr, data_len);
    WASM_ADDR_CHECK(inst, out_ptr, TC_SHA256_DIGEST_SIZE);

    struct tc_sha256_state_struct ctx;
    if (tc_sha256_init(&ctx) != TC_CRYPTO_SUCCESS) {
        return -EIO;
    }
    tc_sha256_update(&ctx, (const uint8_t *)data_ptr, data_len);
    tc_sha256_final((uint8_t *)out_ptr, &ctx);
    return 0;
}

/* ── aes256_cbc_encrypt ─────────────────────────────────────────────────── */

int akira_native_crypto_aes256_encrypt(wasm_exec_env_t exec_env,
                                        void *key_ptr, void *iv_ptr,
                                        void *in_ptr, uint32_t in_len,
                                        void *out_ptr)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

    if (in_len == 0 || (in_len % TC_AES_BLOCK_SIZE) != 0) {
        return -EINVAL;
    }
    if (in_len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
        return -EMSGSIZE;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, key_ptr, TC_AES_KEY_SIZE_256);
    WASM_ADDR_CHECK(inst, iv_ptr,  TC_AES_BLOCK_SIZE);
    WASM_ADDR_CHECK(inst, in_ptr,  in_len);
    WASM_ADDR_CHECK(inst, out_ptr, in_len);

    struct tc_aes_key_sched_struct sched;
    if (tc_aes256_set_encrypt_key(&sched, (const uint8_t *)key_ptr)
        != TC_CRYPTO_SUCCESS) {
        return -EIO;
    }

    int ret = tc_cbc_mode_encrypt((uint8_t *)out_ptr,
                                   in_len,
                                   (const uint8_t *)in_ptr,
                                   in_len,
                                   (const uint8_t *)iv_ptr,
                                   &sched);

    /* Zero key schedule to prevent leaking key material */
    memset(&sched, 0, sizeof(sched));
    return (ret == TC_CRYPTO_SUCCESS) ? 0 : -EIO;
}

/* ── aes256_cbc_decrypt ─────────────────────────────────────────────────── */

int akira_native_crypto_aes256_decrypt(wasm_exec_env_t exec_env,
                                        void *key_ptr, void *iv_ptr,
                                        void *in_ptr, uint32_t in_len,
                                        void *out_ptr)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

    if (in_len == 0 || (in_len % TC_AES_BLOCK_SIZE) != 0) {
        return -EINVAL;
    }
    if (in_len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
        return -EMSGSIZE;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, key_ptr, TC_AES_KEY_SIZE_256);
    WASM_ADDR_CHECK(inst, iv_ptr,  TC_AES_BLOCK_SIZE);
    WASM_ADDR_CHECK(inst, in_ptr,  in_len);
    WASM_ADDR_CHECK(inst, out_ptr, in_len);

    struct tc_aes_key_sched_struct sched;
    if (tc_aes256_set_decrypt_key(&sched, (const uint8_t *)key_ptr)
        != TC_CRYPTO_SUCCESS) {
        return -EIO;
    }

    int ret = tc_cbc_mode_decrypt((uint8_t *)out_ptr,
                                   in_len,
                                   (const uint8_t *)in_ptr,
                                   in_len,
                                   (const uint8_t *)iv_ptr,
                                   &sched);

    memset(&sched, 0, sizeof(sched));
    return (ret == TC_CRYPTO_SUCCESS) ? 0 : -EIO;
}

/* ── hmac_sha256 ────────────────────────────────────────────────────────── */

int akira_native_crypto_hmac_sha256(wasm_exec_env_t exec_env,
                                     void *key_ptr, uint32_t key_len,
                                     void *data_ptr, uint32_t data_len,
                                     void *out_ptr)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

    if (key_len == 0 || key_len > TC_SHA256_BLOCK_SIZE) {
        return -EINVAL;
    }
    if (data_len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
        return -EMSGSIZE;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, key_ptr,  key_len);
    WASM_ADDR_CHECK(inst, data_ptr, data_len);
    WASM_ADDR_CHECK(inst, out_ptr,  TC_SHA256_DIGEST_SIZE);

    struct tc_hmac_state_struct ctx;
    if (tc_hmac_set_key(&ctx, (const uint8_t *)key_ptr, key_len)
        != TC_CRYPTO_SUCCESS) {
        return -EIO;
    }
    tc_hmac_init(&ctx);
    tc_hmac_update(&ctx, (const uint8_t *)data_ptr, data_len);
    if (tc_hmac_final((uint8_t *)out_ptr, TC_SHA256_DIGEST_SIZE, &ctx)
        != TC_CRYPTO_SUCCESS) {
        return -EIO;
    }
    /* Zero HMAC state */
    memset(&ctx, 0, sizeof(ctx));
    return 0;
}

/* ── random_bytes ───────────────────────────────────────────────────────── */

int akira_native_crypto_random(wasm_exec_env_t exec_env,
                                void *buf_ptr, uint32_t len)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

    if (len == 0 || len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, buf_ptr, len);

    sys_csrand_get(buf_ptr, len);
    return 0;
}

#endif /* CONFIG_AKIRA_WASM_CRYPTO */
