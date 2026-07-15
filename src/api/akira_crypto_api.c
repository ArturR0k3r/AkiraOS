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
#include <tinycrypt/ctr_mode.h>
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
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

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
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

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
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

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
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

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
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

    if (len == 0 || len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, buf_ptr, len);

    sys_csrand_get(buf_ptr, len);
    return 0;
}

/* ── aes256_ctr ─────────────────────────────────────────────────────────── */

int akira_native_crypto_aes256_ctr(wasm_exec_env_t exec_env,
                                    void *key_ptr, void *nonce_ptr,
                                    void *in_ptr, uint32_t in_len,
                                    void *out_ptr)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

    if (in_len == 0 || in_len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, key_ptr,   TC_AES_KEY_SIZE_256);
    WASM_ADDR_CHECK(inst, nonce_ptr, TC_AES_BLOCK_SIZE);
    WASM_ADDR_CHECK(inst, in_ptr,    in_len);
    WASM_ADDR_CHECK(inst, out_ptr,   in_len);

    struct tc_aes_key_sched_struct sched;
    if (tc_aes256_set_encrypt_key(&sched, (const uint8_t *)key_ptr)
        != TC_CRYPTO_SUCCESS) {
        return -EIO;
    }

    /* TinyCrypt CTR mode: nonce is the initial counter block (16 bytes).
     * tc_ctr_mode() increments the counter in-place; use a local copy so
     * the caller's nonce buffer is not modified. */
    uint8_t ctr[TC_AES_BLOCK_SIZE];
    memcpy(ctr, nonce_ptr, TC_AES_BLOCK_SIZE);

    int ret = tc_ctr_mode((uint8_t *)out_ptr, in_len,
                          (const uint8_t *)in_ptr, in_len,
                          ctr, &sched);

    memset(&sched, 0, sizeof(sched));
    memset(ctr, 0, sizeof(ctr));
    return (ret == TC_CRYPTO_SUCCESS) ? 0 : -EIO;
}

/* ── ed25519_keygen / ed25519_sign ──────────────────────────────────────── */

#if defined(CONFIG_AKIRA_WASM_CRYPTO_ED25519)
#include <psa/crypto.h>

/*
 * Lazily initialise PSA Crypto (idempotent after first call).
 * mbedTLS for WiFi/BLE has usually already called psa_crypto_init(), but
 * calling it again is safe.
 */
static int psa_ensure_init(void)
{
	psa_status_t s = psa_crypto_init();
	return (s == PSA_SUCCESS || s == PSA_ERROR_ALREADY_EXISTS) ? 0 : -EIO;
}

int akira_native_crypto_ed25519_keygen(wasm_exec_env_t exec_env,
                                        void *seed_ptr, void *pub_ptr)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

	wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
	WASM_ADDR_CHECK(inst, seed_ptr, 32);
	WASM_ADDR_CHECK(inst, pub_ptr,  32);

	if (psa_ensure_init() != 0) return -EIO;

	/* Generate 32 random bytes from hardware RNG as Ed25519 seed */
	sys_csrand_get(seed_ptr, 32);

	/* Derive public key from seed via PSA */
	psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attrs,
	    PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_TWISTED_EDWARDS));
	psa_set_key_bits(&attrs, 255);
	psa_set_key_usage_flags(&attrs,
	    PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
	psa_set_key_algorithm(&attrs, PSA_ALG_PURE_EDDSA);

	psa_key_id_t key_id;
	psa_status_t s = psa_import_key(&attrs,
	                                 (const uint8_t *)seed_ptr, 32,
	                                 &key_id);
	if (s != PSA_SUCCESS) {
		LOG_ERR("ed25519_keygen: import_key failed: %d", (int)s);
		memset(seed_ptr, 0, 32);
		return -EIO;
	}

	size_t pub_len;
	s = psa_export_public_key(key_id, (uint8_t *)pub_ptr, 32, &pub_len);
	psa_destroy_key(key_id);

	if (s != PSA_SUCCESS || pub_len != 32) {
		LOG_ERR("ed25519_keygen: export_public_key failed: %d", (int)s);
		memset(seed_ptr, 0, 32);
		return -EIO;
	}

	LOG_INF("Ed25519 key pair generated");
	return 0;
}

int akira_native_crypto_ed25519_sign(wasm_exec_env_t exec_env,
                                      void *seed_ptr,
                                      void *msg_ptr, uint32_t msg_len,
                                      void *sig_ptr)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

	if (msg_len == 0 || msg_len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
		return -EINVAL;
	}

	wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
	WASM_ADDR_CHECK(inst, seed_ptr, 32);
	WASM_ADDR_CHECK(inst, msg_ptr,  msg_len);
	WASM_ADDR_CHECK(inst, sig_ptr,  64);

	if (psa_ensure_init() != 0) return -EIO;

	psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attrs,
	    PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_TWISTED_EDWARDS));
	psa_set_key_bits(&attrs, 255);
	psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attrs, PSA_ALG_PURE_EDDSA);

	/* Import seed into a volatile key slot */
	psa_key_id_t key_id;
	psa_status_t s = psa_import_key(&attrs,
	                                 (const uint8_t *)seed_ptr, 32,
	                                 &key_id);
	if (s != PSA_SUCCESS) {
		LOG_ERR("ed25519_sign: import_key failed: %d", (int)s);
		return -EIO;
	}

	size_t sig_len;
	s = psa_sign_message(key_id, PSA_ALG_PURE_EDDSA,
	                     (const uint8_t *)msg_ptr, msg_len,
	                     (uint8_t *)sig_ptr, 64, &sig_len);

	/* Destroy key slot immediately — seed never lingers on host */
	psa_destroy_key(key_id);

	if (s != PSA_SUCCESS || sig_len != 64) {
		LOG_ERR("ed25519_sign: sign_message failed: %d", (int)s);
		return -EIO;
	}

	return 0;
}

#else /* !CONFIG_AKIRA_WASM_CRYPTO_ED25519 */

int akira_native_crypto_ed25519_keygen(wasm_exec_env_t exec_env,
                                        void *seed_ptr, void *pub_ptr)
{
	(void)exec_env; (void)seed_ptr; (void)pub_ptr;
	return -ENOTSUP;
}

int akira_native_crypto_ed25519_sign(wasm_exec_env_t exec_env,
                                      void *seed_ptr,
                                      void *msg_ptr, uint32_t msg_len,
                                      void *sig_ptr)
{
	(void)exec_env; (void)seed_ptr; (void)msg_ptr;
	(void)msg_len; (void)sig_ptr;
	return -ENOTSUP;
}

#endif /* CONFIG_AKIRA_WASM_CRYPTO_ED25519 */

#endif /* CONFIG_AKIRA_WASM_CRYPTO */
