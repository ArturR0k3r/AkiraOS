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
#include <zephyr/init.h>
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
#include "ed25519.h"

/*
 * This mbedTLS vendor drop has no twisted-edwards curve in mbedtls_ecp
 * (see mbedtls/ecp.h mbedtls_ecp_group_id — only Weierstrass + Curve25519/
 * 448 Montgomery), so PSA_ECC_FAMILY_TWISTED_EDWARDS / PSA_ALG_PURE_EDDSA
 * are unimplemented spec constants only; psa_import_key() always returns
 * PSA_ERROR_NOT_SUPPORTED for them. ed25519.c implements RFC 8032 directly
 * on mbedtls_mpi + mbedtls_sha512 instead of going through PSA.
 */

int akira_native_crypto_ed25519_keygen(wasm_exec_env_t exec_env,
                                        void *seed_ptr, void *pub_ptr)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

	wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
	WASM_ADDR_CHECK(inst, seed_ptr, 32);
	WASM_ADDR_CHECK(inst, pub_ptr,  32);

	/* Generate 32 random bytes from hardware RNG as Ed25519 seed */
	sys_csrand_get(seed_ptr, 32);

	int ret = ed25519_keygen((const uint8_t *)seed_ptr, (uint8_t *)pub_ptr);
	if (ret != 0) {
		LOG_ERR("ed25519_keygen failed: %d", ret);
		memset(seed_ptr, 0, 32);
		return ret;
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

	int ret = ed25519_sign((const uint8_t *)seed_ptr,
	                        (const uint8_t *)msg_ptr, msg_len,
	                        (uint8_t *)sig_ptr);
	if (ret != 0) {
		LOG_ERR("ed25519_sign failed: %d", ret);
		return ret;
	}

	return 0;
}

#ifdef CONFIG_AKIRA_CRYPTO_ED25519_SELFTEST
static int ed25519_boot_self_test(void)
{
	int ret = ed25519_self_test();
	if (ret != 0) {
		LOG_ERR("ed25519_self_test FAILED: %d", ret);
	} else {
		LOG_INF("ed25519_self_test PASSED (RFC 8032 test vector 1)");
	}
	return 0;
}
SYS_INIT(ed25519_boot_self_test, APPLICATION, 90);
#endif /* CONFIG_AKIRA_CRYPTO_ED25519_SELFTEST */

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

/* ── p256_keygen / p256_sign (U2F) ────────────────────────────────────────
 * Unlike Ed25519, this mbedTLS vendor drop DOES have secp256r1/P-256 (it's
 * a Weierstrass curve — see mbedtls_ecp_group_id), and PSA_WANT_ECC_SECP_R1_256
 * / PSA_WANT_ALG_ECDSA are real, wired want-symbols here (confirmed present
 * in Kconfig.psa.auto + configs/config-psa.h, unlike the missing twisted-
 * edwards symbols that forced ed25519.c to be vendored standalone). So this
 * goes through PSA properly instead of a hand-rolled implementation.
 */

#if defined(CONFIG_AKIRA_WASM_CRYPTO_ECDSA_P256)
#include <psa/crypto.h>

int akira_native_crypto_p256_keygen(wasm_exec_env_t exec_env,
                                     void *priv_ptr, void *pub_ptr)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

	wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
	WASM_ADDR_CHECK(inst, priv_ptr, 32);
	WASM_ADDR_CHECK(inst, pub_ptr,  65);

	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

	mbedtls_svc_key_id_t key_id;
	psa_status_t st = psa_generate_key(&attr, &key_id);
	if (st != PSA_SUCCESS) {
		LOG_ERR("p256 psa_generate_key failed: %d", (int)st);
		return -EIO;
	}

	size_t priv_len = 0, pub_len = 0;
	st = psa_export_key(key_id, (uint8_t *)priv_ptr, 32, &priv_len);
	if (st == PSA_SUCCESS) {
		st = psa_export_public_key(key_id, (uint8_t *)pub_ptr, 65, &pub_len);
	}
	psa_destroy_key(key_id);

	if (st != PSA_SUCCESS || priv_len != 32 || pub_len != 65) {
		LOG_ERR("p256 export failed: %d", (int)st);
		memset(priv_ptr, 0, 32);
		return -EIO;
	}

	LOG_INF("P-256 key pair generated");
	return 0;
}

int akira_native_crypto_p256_sign(wasm_exec_env_t exec_env,
                                   void *priv_ptr,
                                   void *msg_ptr, uint32_t msg_len,
                                   void *sig_ptr)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

	if (msg_len == 0 || msg_len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
		return -EINVAL;
	}

	wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
	WASM_ADDR_CHECK(inst, priv_ptr, 32);
	WASM_ADDR_CHECK(inst, msg_ptr,  msg_len);
	WASM_ADDR_CHECK(inst, sig_ptr,  64);

	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

	mbedtls_svc_key_id_t key_id;
	psa_status_t st = psa_import_key(&attr, (const uint8_t *)priv_ptr, 32, &key_id);
	if (st != PSA_SUCCESS) {
		LOG_ERR("p256 psa_import_key failed: %d", (int)st);
		return -EIO;
	}

	size_t sig_len = 0;
	st = psa_sign_message(key_id, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
	                       (const uint8_t *)msg_ptr, msg_len,
	                       (uint8_t *)sig_ptr, 64, &sig_len);
	psa_destroy_key(key_id);

	if (st != PSA_SUCCESS || sig_len != 64) {
		LOG_ERR("p256 psa_sign_message failed: %d", (int)st);
		return -EIO;
	}

	return 0;
}

#else /* !CONFIG_AKIRA_WASM_CRYPTO_ECDSA_P256 */

int akira_native_crypto_p256_keygen(wasm_exec_env_t exec_env,
                                     void *priv_ptr, void *pub_ptr)
{
	(void)exec_env; (void)priv_ptr; (void)pub_ptr;
	return -ENOTSUP;
}

int akira_native_crypto_p256_sign(wasm_exec_env_t exec_env,
                                   void *priv_ptr,
                                   void *msg_ptr, uint32_t msg_len,
                                   void *sig_ptr)
{
	(void)exec_env; (void)priv_ptr; (void)msg_ptr;
	(void)msg_len; (void)sig_ptr;
	return -ENOTSUP;
}

#endif /* CONFIG_AKIRA_WASM_CRYPTO_ECDSA_P256 */

/* ── SE050 hardware-backed P-256 (non-exportable keys) — U2F offload ──────── */
#ifdef CONFIG_AKIRA_SE050_U2F
#include "../drivers/secure_element/se050.h"

#define SE050_U2F_MAX_SLOT   255

int akira_native_crypto_p256_keygen_se050(wasm_exec_env_t exec_env,
                                          uint32_t slot, void *pub_ptr)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

	if (slot > SE050_U2F_MAX_SLOT) {
		return -EINVAL;
	}
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
	WASM_ADDR_CHECK(inst, pub_ptr, SE050_P256_PUB_LEN);

	const struct device *se = se050_get_device();
	if (!se) {
		return -ENODEV;
	}

	uint32_t objid = (uint32_t)CONFIG_AKIRA_SE050_U2F_KEY_OBJID + slot;
	int ret = se050_ecc_gen_key(se, objid);
	if (ret < 0) {
		return ret;
	}
	return se050_ecc_get_pub(se, objid, (uint8_t *)pub_ptr);
}

int akira_native_crypto_p256_sign_se050(wasm_exec_env_t exec_env,
                                        uint32_t slot,
                                        void *hash_ptr, void *sig_ptr)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EACCES);

	if (slot > SE050_U2F_MAX_SLOT) {
		return -EINVAL;
	}
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
	WASM_ADDR_CHECK(inst, hash_ptr, 32);
	WASM_ADDR_CHECK(inst, sig_ptr,  SE050_P256_SIG_LEN);

	const struct device *se = se050_get_device();
	if (!se) {
		return -ENODEV;
	}

	uint32_t objid = (uint32_t)CONFIG_AKIRA_SE050_U2F_KEY_OBJID + slot;
	return se050_ecc_sign(se, objid, (const uint8_t *)hash_ptr,
	                      (uint8_t *)sig_ptr);
}

#else /* !CONFIG_AKIRA_SE050_U2F */

int akira_native_crypto_p256_keygen_se050(wasm_exec_env_t exec_env,
                                          uint32_t slot, void *pub_ptr)
{
	(void)exec_env; (void)slot; (void)pub_ptr;
	return -ENOTSUP;
}

int akira_native_crypto_p256_sign_se050(wasm_exec_env_t exec_env,
                                        uint32_t slot,
                                        void *hash_ptr, void *sig_ptr)
{
	(void)exec_env; (void)slot; (void)hash_ptr; (void)sig_ptr;
	return -ENOTSUP;
}

#endif /* CONFIG_AKIRA_SE050_U2F */

#endif /* CONFIG_AKIRA_WASM_CRYPTO */
