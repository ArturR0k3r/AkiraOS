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
 * Backend: mbedTLS (already compiled in for WiFi/BLE on ESP32-S3).
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

#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>
#include <zephyr/random/random.h>

#define TC_SHA256_DIGEST_SIZE  32
#define TC_AES_KEY_SIZE_256    32
#define TC_AES_BLOCK_SIZE      16

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

    int ret = mbedtls_sha256((const uint8_t *)data_ptr, data_len,
                              (uint8_t *)out_ptr, 0 /* 0 = SHA-256 */);
    return (ret == 0) ? 0 : -EIO;
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

    /* mbedTLS crypt_cbc modifies iv in-place; use a local copy */
    uint8_t iv_local[TC_AES_BLOCK_SIZE];
    memcpy(iv_local, iv_ptr, TC_AES_BLOCK_SIZE);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int ret = mbedtls_aes_setkey_enc(&ctx, (const uint8_t *)key_ptr, 256);
    if (ret == 0) {
        ret = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT,
                                     in_len, iv_local,
                                     (const uint8_t *)in_ptr,
                                     (uint8_t *)out_ptr);
    }
    mbedtls_aes_free(&ctx);
    memset(iv_local, 0, sizeof(iv_local));
    return (ret == 0) ? 0 : -EIO;
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

    uint8_t iv_local[TC_AES_BLOCK_SIZE];
    memcpy(iv_local, iv_ptr, TC_AES_BLOCK_SIZE);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int ret = mbedtls_aes_setkey_dec(&ctx, (const uint8_t *)key_ptr, 256);
    if (ret == 0) {
        ret = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT,
                                     in_len, iv_local,
                                     (const uint8_t *)in_ptr,
                                     (uint8_t *)out_ptr);
    }
    mbedtls_aes_free(&ctx);
    memset(iv_local, 0, sizeof(iv_local));
    return (ret == 0) ? 0 : -EIO;
}

/* ── hmac_sha256 ────────────────────────────────────────────────────────── */

int akira_native_crypto_hmac_sha256(wasm_exec_env_t exec_env,
                                     void *key_ptr, uint32_t key_len,
                                     void *data_ptr, uint32_t data_len,
                                     void *out_ptr)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

    if (key_len == 0 || key_len > 64u) { /* SHA256 block size */
        return -EINVAL;
    }
    if (data_len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
        return -EMSGSIZE;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, key_ptr,  key_len);
    WASM_ADDR_CHECK(inst, data_ptr, data_len);
    WASM_ADDR_CHECK(inst, out_ptr,  TC_SHA256_DIGEST_SIZE);

    /* HMAC-SHA256 built from raw mbedtls_sha256 (avoids MBEDTLS_MD_C dep) */
    #define HMAC_BLOCK 64
    uint8_t k_ipad[HMAC_BLOCK], k_opad[HMAC_BLOCK];
    memset(k_ipad, 0x36, HMAC_BLOCK);
    memset(k_opad, 0x5c, HMAC_BLOCK);
    for (uint32_t i = 0; i < key_len; i++) {
        k_ipad[i] ^= ((const uint8_t *)key_ptr)[i];
        k_opad[i] ^= ((const uint8_t *)key_ptr)[i];
    }
    uint8_t inner[TC_SHA256_DIGEST_SIZE];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, k_ipad, HMAC_BLOCK);
    mbedtls_sha256_update(&ctx, (const uint8_t *)data_ptr, data_len);
    mbedtls_sha256_finish(&ctx, inner);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, k_opad, HMAC_BLOCK);
    mbedtls_sha256_update(&ctx, inner, TC_SHA256_DIGEST_SIZE);
    mbedtls_sha256_finish(&ctx, (uint8_t *)out_ptr);
    mbedtls_sha256_free(&ctx);
    memset(k_ipad, 0, HMAC_BLOCK);
    memset(k_opad, 0, HMAC_BLOCK);
    memset(inner,  0, sizeof(inner));
    return 0;
    #undef HMAC_BLOCK
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

/* ── aes256_ctr ─────────────────────────────────────────────────────────── */

int akira_native_crypto_aes256_ctr(wasm_exec_env_t exec_env,
                                    void *key_ptr, void *nonce_ptr,
                                    void *in_ptr, uint32_t in_len,
                                    void *out_ptr)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

    if (in_len == 0 || in_len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
        return -EINVAL;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, key_ptr,   TC_AES_KEY_SIZE_256);
    WASM_ADDR_CHECK(inst, nonce_ptr, TC_AES_BLOCK_SIZE);
    WASM_ADDR_CHECK(inst, in_ptr,    in_len);
    WASM_ADDR_CHECK(inst, out_ptr,   in_len);

    /* Use local nonce/stream-block copies so caller's buffer is not modified */
    uint8_t nonce_ctr[TC_AES_BLOCK_SIZE];
    uint8_t stream_block[TC_AES_BLOCK_SIZE];
    size_t  nc_off = 0;
    memcpy(nonce_ctr, nonce_ptr, TC_AES_BLOCK_SIZE);
    memset(stream_block, 0, sizeof(stream_block));

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int ret = mbedtls_aes_setkey_enc(&ctx, (const uint8_t *)key_ptr, 256);
    if (ret == 0) {
        ret = mbedtls_aes_crypt_ctr(&ctx, in_len, &nc_off,
                                     nonce_ctr, stream_block,
                                     (const uint8_t *)in_ptr,
                                     (uint8_t *)out_ptr);
    }
    mbedtls_aes_free(&ctx);
    memset(nonce_ctr,   0, sizeof(nonce_ctr));
    memset(stream_block, 0, sizeof(stream_block));
    return (ret == 0) ? 0 : -EIO;
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
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

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
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

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
