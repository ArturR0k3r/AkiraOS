/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_crypto_api
#include <zephyr/logging/log.h>
#include <crystal/akira_seal.h>
LOG_MODULE_REGISTER(akira_crypto_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_crypto_api.c
 * @brief Cryptographic operations WASM native API.
 *
 * Backend: mbedTLS (SHA-256 and the AES block cipher), the in-tree
 * Ed25519 code (src/lib/crypto/ed25519) and sys_csrand_get().
 * - Key material is zeroed after use; keys and seeds only ever sit in
 *   stack buffers, never in shared static buffers.
 * - All input lengths capped at CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT (default 64KB).
 * - Every operation validates WASM linear memory bounds (full lengths)
 *   before use, and writes only inside the validated output ranges.
 * - Each operation is a bounded native critical section
 *   (akira_app_containment.h).
 *
 * The CBC and CTR modes are built here on the AES block function, so their
 * results match the TinyCrypt backend this API used before byte for byte:
 * CTR increments only the last 4 bytes of the counter block (big-endian,
 * wrapping), as TinyCrypt did.
 *
 * Gate: CONFIG_AKIRA_WASM_CRYPTO=y
 * Capability: AKIRA_CAP_CRYPTO (bit 28)
 */

#ifdef CONFIG_AKIRA_WASM_CRYPTO

#include "akira_crypto_api.h"
#include <akira_app_containment.h>
#include <runtime/security.h>
#include <runtime/akira_runtime.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>

#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform_util.h>

#ifndef CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT
#define CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT (64u * 1024u)
#endif

#define AES_BLOCK        16u
#define AES256_KEY_LEN   32u
#define SHA256_LEN       32u
#define SHA256_BLOCK_LEN 64u

/* ── helpers ────────────────────────────────────────────────────────────── */

/* Every operation below runs as a native critical section: the AES or SHA
 * engine may sit behind a driver lock (hardware acceleration), the entropy
 * driver may hold its own lock, and the key schedule must reach its
 * zeroization. A stop must never abort the app in between. The work is
 * bounded by CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT (<= 128 KiB, tens of ms). */
#define CRYPTO_CRITICAL_BEGIN(env) akira_native_critical_enter(env)
#define CRYPTO_CRITICAL_END(env)   akira_native_critical_exit(env)

/** Validate a WASM pointer/length and abort on failure. */
#define WASM_ADDR_CHECK(inst, ptr, len)                                 \
    do {                                                                 \
        if (!(ptr) || !wasm_runtime_validate_native_addr((inst),        \
                                                          (ptr), (len)))\
        { return -EFAULT; }                                              \
    } while (0)

/* One AES block through separate buffers, so it doesn't depend on the
 * backend (software or a hardware driver) supporting in == out. */
static int aes_block(mbedtls_aes_context *ctx, int mode,
                     const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK])
{
    return mbedtls_aes_crypt_ecb(ctx, mode, in, out);
}

/*
 * AES-256-CBC over whole blocks, one block at a time, with the IV in its
 * own buffer. Each input block is copied out before its output block is
 * written, so out == in (in place) works.
 */
static int cbc_encrypt_blocks(mbedtls_aes_context *ctx, uint8_t *out,
                              const uint8_t *in, uint32_t len,
                              const uint8_t iv[AES_BLOCK])
{
    uint8_t chain[AES_BLOCK];
    uint8_t enc[AES_BLOCK];
    int ret = 0;

    memcpy(chain, iv, sizeof(chain));
    for (uint32_t off = 0; off < len; off += AES_BLOCK) {
        for (uint32_t i = 0; i < AES_BLOCK; i++) {
            chain[i] ^= in[off + i];
        }
        ret = aes_block(ctx, MBEDTLS_AES_ENCRYPT, chain, enc);
        if (ret != 0) {
            break;
        }
        memcpy(chain, enc, AES_BLOCK);
        memcpy(&out[off], enc, AES_BLOCK);
    }
    mbedtls_platform_zeroize(chain, sizeof(chain));
    mbedtls_platform_zeroize(enc, sizeof(enc));
    return ret;
}

static int cbc_decrypt_blocks(mbedtls_aes_context *ctx, uint8_t *out,
                              const uint8_t *in, uint32_t len,
                              const uint8_t iv[AES_BLOCK])
{
    uint8_t prev[AES_BLOCK];
    uint8_t cblk[AES_BLOCK];
    uint8_t pblk[AES_BLOCK];
    int ret = 0;

    memcpy(prev, iv, sizeof(prev));
    for (uint32_t off = 0; off < len; off += AES_BLOCK) {
        memcpy(cblk, &in[off], AES_BLOCK);
        ret = aes_block(ctx, MBEDTLS_AES_DECRYPT, cblk, pblk);
        if (ret != 0) {
            break;
        }
        for (uint32_t i = 0; i < AES_BLOCK; i++) {
            out[off + i] = pblk[i] ^ prev[i];
        }
        memcpy(prev, cblk, AES_BLOCK);
    }
    mbedtls_platform_zeroize(prev, sizeof(prev));
    mbedtls_platform_zeroize(cblk, sizeof(cblk));
    mbedtls_platform_zeroize(pblk, sizeof(pblk));
    return ret;
}

/*
 * AES-CTR with TinyCrypt's counter rule: bytes 12..15 of the counter block
 * are a big-endian 32-bit block number, incremented per block and wrapping
 * without carrying into byte 11. Reads in[i] before writing out[i], so in
 * place works.
 */
static int ctr_crypt(mbedtls_aes_context *ctx, uint8_t *out,
                     const uint8_t *in, uint32_t len,
                     const uint8_t counter[AES_BLOCK])
{
    uint8_t ctr[AES_BLOCK];
    uint8_t stream[AES_BLOCK];
    uint32_t block_num;
    int ret = 0;

    memcpy(ctr, counter, sizeof(ctr));
    block_num = ((uint32_t)ctr[12] << 24) | ((uint32_t)ctr[13] << 16) |
                ((uint32_t)ctr[14] << 8) | (uint32_t)ctr[15];

    for (uint32_t i = 0; i < len; i++) {
        if ((i % AES_BLOCK) == 0) {
            ret = aes_block(ctx, MBEDTLS_AES_ENCRYPT, ctr, stream);
            if (ret != 0) {
                break;
            }
            block_num++;
            ctr[12] = (uint8_t)(block_num >> 24);
            ctr[13] = (uint8_t)(block_num >> 16);
            ctr[14] = (uint8_t)(block_num >> 8);
            ctr[15] = (uint8_t)block_num;
        }
        out[i] = stream[i % AES_BLOCK] ^ in[i];
    }
    mbedtls_platform_zeroize(ctr, sizeof(ctr));
    mbedtls_platform_zeroize(stream, sizeof(stream));
    return ret;
}

/* HMAC-SHA256 (RFC 2104) for keys of at most one block (64 bytes), which
 * the API enforces. Reads all of data before writing out, so they may
 * overlap. */
static int hmac_sha256(const uint8_t *key, uint32_t key_len,
                       const uint8_t *data, uint32_t data_len,
                       uint8_t out[SHA256_LEN])
{
    mbedtls_sha256_context sha;
    uint8_t pad[SHA256_BLOCK_LEN];
    uint8_t inner[SHA256_LEN];
    int ret;

    memset(pad, 0x36, sizeof(pad));
    for (uint32_t i = 0; i < key_len; i++) {
        pad[i] ^= key[i];
    }

    mbedtls_sha256_init(&sha);
    ret = mbedtls_sha256_starts(&sha, 0);
    if (ret == 0) {
        ret = mbedtls_sha256_update(&sha, pad, sizeof(pad));
    }
    if (ret == 0 && data_len) {
        ret = mbedtls_sha256_update(&sha, data, data_len);
    }
    if (ret == 0) {
        ret = mbedtls_sha256_finish(&sha, inner);
    }
    mbedtls_sha256_free(&sha);

    if (ret == 0) {
        for (uint32_t i = 0; i < sizeof(pad); i++) {
            pad[i] ^= 0x36 ^ 0x5c;
        }
        mbedtls_sha256_init(&sha);
        ret = mbedtls_sha256_starts(&sha, 0);
        if (ret == 0) {
            ret = mbedtls_sha256_update(&sha, pad, sizeof(pad));
        }
        if (ret == 0) {
            ret = mbedtls_sha256_update(&sha, inner, sizeof(inner));
        }
        if (ret == 0) {
            ret = mbedtls_sha256_finish(&sha, out);
        }
        mbedtls_sha256_free(&sha);
    }

    mbedtls_platform_zeroize(pad, sizeof(pad));
    mbedtls_platform_zeroize(inner, sizeof(inner));
    return ret;
}

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
    WASM_ADDR_CHECK(inst, out_ptr, SHA256_LEN);

    uint8_t digest[SHA256_LEN];
    int ret;

    CRYPTO_CRITICAL_BEGIN(exec_env);
    ret = mbedtls_sha256((const uint8_t *)data_ptr, data_len, digest, 0);
    if (ret == 0) {
        memcpy(out_ptr, digest, sizeof(digest));
    }
    CRYPTO_CRITICAL_END(exec_env);
    return (ret == 0) ? 0 : -EIO;
}

/* ── aes256_cbc_encrypt / aes256_cbc_decrypt ────────────────────────────── */

static int aes256_cbc(wasm_exec_env_t exec_env, bool encrypt,
                      void *key_ptr, void *iv_ptr,
                      void *in_ptr, uint32_t in_len, void *out_ptr)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

    if (in_len == 0 || (in_len % AES_BLOCK) != 0) {
        return -EINVAL;
    }
    if (in_len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
        return -EMSGSIZE;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, key_ptr, AES256_KEY_LEN);
    WASM_ADDR_CHECK(inst, iv_ptr,  AES_BLOCK);
    WASM_ADDR_CHECK(inst, in_ptr,  in_len);
    WASM_ADDR_CHECK(inst, out_ptr, in_len);

    mbedtls_aes_context ctx;
    uint8_t key[AES256_KEY_LEN];
    uint8_t iv[AES_BLOCK];
    int ret;

    CRYPTO_CRITICAL_BEGIN(exec_env);
    /* Key and IV are copied first: the output may overlap them. */
    memcpy(key, key_ptr, sizeof(key));
    memcpy(iv, iv_ptr, sizeof(iv));
    mbedtls_aes_init(&ctx);
    ret = encrypt ? mbedtls_aes_setkey_enc(&ctx, key, 256)
                  : mbedtls_aes_setkey_dec(&ctx, key, 256);
    if (ret == 0) {
        ret = encrypt
            ? cbc_encrypt_blocks(&ctx, (uint8_t *)out_ptr,
                                 (const uint8_t *)in_ptr, in_len, iv)
            : cbc_decrypt_blocks(&ctx, (uint8_t *)out_ptr,
                                 (const uint8_t *)in_ptr, in_len, iv);
    }
    mbedtls_aes_free(&ctx);
    mbedtls_platform_zeroize(&ctx, sizeof(ctx));
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(iv, sizeof(iv));
    CRYPTO_CRITICAL_END(exec_env);
    return (ret == 0) ? 0 : -EIO;
}

int akira_native_crypto_aes256_encrypt(wasm_exec_env_t exec_env,
                                        void *key_ptr, void *iv_ptr,
                                        void *in_ptr, uint32_t in_len,
                                        void *out_ptr)
{
    return aes256_cbc(exec_env, true, key_ptr, iv_ptr, in_ptr, in_len, out_ptr);
}

int akira_native_crypto_aes256_decrypt(wasm_exec_env_t exec_env,
                                        void *key_ptr, void *iv_ptr,
                                        void *in_ptr, uint32_t in_len,
                                        void *out_ptr)
{
    return aes256_cbc(exec_env, false, key_ptr, iv_ptr, in_ptr, in_len, out_ptr);
}

/* ── hmac_sha256 ────────────────────────────────────────────────────────── */

int akira_native_crypto_hmac_sha256(wasm_exec_env_t exec_env,
                                     void *key_ptr, uint32_t key_len,
                                     void *data_ptr, uint32_t data_len,
                                     void *out_ptr)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

    if (key_len == 0 || key_len > SHA256_BLOCK_LEN) {
        return -EINVAL;
    }
    if (data_len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
        return -EMSGSIZE;
    }

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASM_ADDR_CHECK(inst, key_ptr,  key_len);
    WASM_ADDR_CHECK(inst, data_ptr, data_len);
    WASM_ADDR_CHECK(inst, out_ptr,  SHA256_LEN);

    uint8_t key[SHA256_BLOCK_LEN];
    uint8_t mac[SHA256_LEN];
    int ret;

    CRYPTO_CRITICAL_BEGIN(exec_env);
    memcpy(key, key_ptr, key_len);
    ret = hmac_sha256(key, key_len, (const uint8_t *)data_ptr, data_len, mac);
    if (ret == 0) {
        memcpy(out_ptr, mac, sizeof(mac));
    }
    mbedtls_platform_zeroize(key, sizeof(key));
    CRYPTO_CRITICAL_END(exec_env);
    return (ret == 0) ? 0 : -EIO;
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

    CRYPTO_CRITICAL_BEGIN(exec_env);
    int ret = sys_csrand_get(buf_ptr, len);
    CRYPTO_CRITICAL_END(exec_env);
    return (ret == 0) ? 0 : -EIO;
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
    WASM_ADDR_CHECK(inst, key_ptr,   AES256_KEY_LEN);
    WASM_ADDR_CHECK(inst, nonce_ptr, AES_BLOCK);
    WASM_ADDR_CHECK(inst, in_ptr,    in_len);
    WASM_ADDR_CHECK(inst, out_ptr,   in_len);

    mbedtls_aes_context ctx;
    uint8_t key[AES256_KEY_LEN];
    /* The nonce is the initial counter block. It is copied, so the
     * caller's buffer is not modified. */
    uint8_t ctr[AES_BLOCK];
    int ret;

    CRYPTO_CRITICAL_BEGIN(exec_env);
    memcpy(key, key_ptr, sizeof(key));
    memcpy(ctr, nonce_ptr, sizeof(ctr));
    mbedtls_aes_init(&ctx);
    ret = mbedtls_aes_setkey_enc(&ctx, key, 256);
    if (ret == 0) {
        ret = ctr_crypt(&ctx, (uint8_t *)out_ptr, (const uint8_t *)in_ptr,
                        in_len, ctr);
    }
    mbedtls_aes_free(&ctx);
    mbedtls_platform_zeroize(&ctx, sizeof(ctx));
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(ctr, sizeof(ctr));
    CRYPTO_CRITICAL_END(exec_env);
    return (ret == 0) ? 0 : -EIO;
}

/* ── ed25519_keygen / ed25519_sign ──────────────────────────────────────── */

#if defined(CONFIG_AKIRA_WASM_CRYPTO_ED25519)
#include <lib/crypto/ed25519/ed25519_sign.h>

int akira_native_crypto_ed25519_keygen(wasm_exec_env_t exec_env,
                                        void *seed_ptr, void *pub_ptr)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

	wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
	WASM_ADDR_CHECK(inst, seed_ptr, 32);
	WASM_ADDR_CHECK(inst, pub_ptr,  32);

	/* Generated and derived in host buffers, then copied out: the app's
	 * buffers are written once, and the seed never stays on the host. */
	uint8_t seed[32];
	uint8_t pub[32];

	CRYPTO_CRITICAL_BEGIN(exec_env);
	int ret = (sys_csrand_get(seed, sizeof(seed)) == 0) ? 0 : -EIO;
	if (ret == 0) {
		ret = akira_ed25519_pubkey_from_seed(seed, pub);
	}
	if (ret == 0) {
		memcpy(seed_ptr, seed, sizeof(seed));
		memcpy(pub_ptr, pub, sizeof(pub));
	}
	mbedtls_platform_zeroize(seed, sizeof(seed));
	CRYPTO_CRITICAL_END(exec_env);

	return (ret == 0) ? 0 : -EIO;
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

	uint8_t seed[32];
	uint8_t sig[64];

	/* Signed into a host buffer and copied out, so the signature may
	 * overlap the message or the seed in app memory. */
	CRYPTO_CRITICAL_BEGIN(exec_env);
	memcpy(seed, seed_ptr, sizeof(seed));
	int ret = akira_ed25519_sign((const uint8_t *)msg_ptr, msg_len, seed, sig);
	if (ret == 0) {
		memcpy(sig_ptr, sig, sizeof(sig));
	}
	mbedtls_platform_zeroize(seed, sizeof(seed));
	CRYPTO_CRITICAL_END(exec_env);

	return (ret == 0) ? 0 : -EIO;
}

#ifdef CONFIG_AKIRA_CRYPTO_ED25519_SELFTEST
/* Known-answer test of the vendored SHA-512 + Ed25519 in src/api/ed25519.c
 * (the AkiraMesh handshake signs with it; the natives above use the in-tree
 * src/lib/crypto/ed25519 code). Costs seconds of CPU on every boot. */
#include <zephyr/init.h>
#include "ed25519.h"

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
 * Unlike Ed25519, mbedTLS has secp256r1/P-256 (a Weierstrass curve), and
 * PSA_WANT_ECC_SECP_R1_256 / PSA_WANT_ALG_ECDSA are wired want-symbols, so
 * this goes through PSA rather than a hand-rolled implementation.
 *
 * Same rules as the operations above: full-length pointer checks, keys only
 * in stack buffers (zeroized), results copied out once, and every PSA call
 * inside a bounded native critical section (PSA holds its own global lock
 * and the key slot must be destroyed).
 */

#if defined(CONFIG_AKIRA_WASM_CRYPTO_ECDSA_P256)
#include <psa/crypto.h>

#define P256_PRIV_LEN 32u
#define P256_PUB_LEN  65u   /* uncompressed 0x04 || X || Y */
#define P256_SIG_LEN  64u   /* raw r || s */

int akira_native_crypto_p256_keygen(wasm_exec_env_t exec_env,
                                     void *priv_ptr, void *pub_ptr)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

	wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
	WASM_ADDR_CHECK(inst, priv_ptr, P256_PRIV_LEN);
	WASM_ADDR_CHECK(inst, pub_ptr,  P256_PUB_LEN);

	uint8_t priv[P256_PRIV_LEN];
	uint8_t pub[P256_PUB_LEN];
	size_t priv_len = 0, pub_len = 0;
	psa_status_t st;

	CRYPTO_CRITICAL_BEGIN(exec_env);
	st = psa_crypto_init();
	if (st == PSA_SUCCESS) {
		psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
		mbedtls_svc_key_id_t key_id;

		psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
		psa_set_key_bits(&attr, 256);
		psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT);
		psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

		st = psa_generate_key(&attr, &key_id);
		if (st == PSA_SUCCESS) {
			st = psa_export_key(key_id, priv, sizeof(priv), &priv_len);
			if (st == PSA_SUCCESS) {
				st = psa_export_public_key(key_id, pub, sizeof(pub), &pub_len);
			}
			psa_destroy_key(key_id);
		}
	}
	bool ok = (st == PSA_SUCCESS && priv_len == P256_PRIV_LEN &&
		   pub_len == P256_PUB_LEN);
	if (ok) {
		memcpy(priv_ptr, priv, sizeof(priv));
		memcpy(pub_ptr, pub, sizeof(pub));
	}
	mbedtls_platform_zeroize(priv, sizeof(priv));
	CRYPTO_CRITICAL_END(exec_env);

	if (!ok) {
		LOG_WRN("p256_keygen failed: %d", (int)st);
		return -EIO;
	}
	return 0;
}

int akira_native_crypto_p256_sign(wasm_exec_env_t exec_env,
                                   void *priv_ptr,
                                   void *msg_ptr, uint32_t msg_len,
                                   void *sig_ptr)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

	if (msg_len == 0 || msg_len > CONFIG_AKIRA_WASM_CRYPTO_MAX_INPUT) {
		return -EINVAL;
	}

	wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
	WASM_ADDR_CHECK(inst, priv_ptr, P256_PRIV_LEN);
	WASM_ADDR_CHECK(inst, msg_ptr,  msg_len);
	WASM_ADDR_CHECK(inst, sig_ptr,  P256_SIG_LEN);

	uint8_t priv[P256_PRIV_LEN];
	uint8_t sig[P256_SIG_LEN];
	size_t sig_len = 0;
	psa_status_t st;

	/* Signed into a host buffer and copied out, so the signature may
	 * overlap the message or the key in app memory. */
	CRYPTO_CRITICAL_BEGIN(exec_env);
	memcpy(priv, priv_ptr, sizeof(priv));
	st = psa_crypto_init();
	if (st == PSA_SUCCESS) {
		psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
		mbedtls_svc_key_id_t key_id;

		psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
		psa_set_key_bits(&attr, 256);
		psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
		psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

		st = psa_import_key(&attr, priv, sizeof(priv), &key_id);
		if (st == PSA_SUCCESS) {
			st = psa_sign_message(key_id, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
					      (const uint8_t *)msg_ptr, msg_len,
					      sig, sizeof(sig), &sig_len);
			psa_destroy_key(key_id);
		}
	}
	bool ok = (st == PSA_SUCCESS && sig_len == P256_SIG_LEN);
	if (ok) {
		memcpy(sig_ptr, sig, sizeof(sig));
	}
	mbedtls_platform_zeroize(priv, sizeof(priv));
	CRYPTO_CRITICAL_END(exec_env);

	if (!ok) {
		LOG_WRN("p256_sign failed: %d", (int)st);
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

/* ── SE050 hardware-backed P-256 (non-exportable keys) — U2F offload ────────
 * Each call is a few APDUs over the SE050's I2C link (bus lock, bounded by
 * the driver's own timeouts): run as a native critical section. Signing
 * with a vault key asks the Seal first, after the arguments are checked. */
#ifdef CONFIG_AKIRA_SE050_U2F
#include <drivers/secure_element/se050.h>

#define SE050_U2F_MAX_SLOT   255

int akira_native_crypto_p256_keygen_se050(wasm_exec_env_t exec_env,
                                          uint32_t slot, void *pub_ptr)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

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
	uint8_t pub[SE050_P256_PUB_LEN];

	CRYPTO_CRITICAL_BEGIN(exec_env);
	int ret = se050_ecc_gen_key(se, objid);
	if (ret >= 0) {
		ret = se050_ecc_get_pub(se, objid, pub);
	}
	if (ret >= 0) {
		memcpy(pub_ptr, pub, sizeof(pub));
	}
	CRYPTO_CRITICAL_END(exec_env);

	return ret;
}

int akira_native_crypto_p256_sign_se050(wasm_exec_env_t exec_env,
                                        uint32_t slot,
                                        void *hash_ptr, void *sig_ptr)
{
	AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_CRYPTO, -EPERM);

	if (slot > SE050_U2F_MAX_SLOT) {
		return -EINVAL;
	}
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
	WASM_ADDR_CHECK(inst, hash_ptr, SHA256_LEN);
	WASM_ADDR_CHECK(inst, sig_ptr,  SE050_P256_SIG_LEN);

	AKIRA_SEAL_GATE_OR_RETURN(exec_env, AKIRA_SEAL_VAULT_SIGN, NULL, -EACCES);

	const struct device *se = se050_get_device();
	if (!se) {
		return -ENODEV;
	}

	uint32_t objid = (uint32_t)CONFIG_AKIRA_SE050_U2F_KEY_OBJID + slot;
	uint8_t hash[SHA256_LEN];
	uint8_t sig[SE050_P256_SIG_LEN];

	CRYPTO_CRITICAL_BEGIN(exec_env);
	memcpy(hash, hash_ptr, sizeof(hash));
	int ret = se050_ecc_sign(se, objid, hash, sig);
	if (ret >= 0) {
		memcpy(sig_ptr, sig, sizeof(sig));
	}
	CRYPTO_CRITICAL_END(exec_env);

	return ret;
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

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_CRYPTO))
#include <akira_native_registry.h>

/* crypto.*: sha256, aes256-cbc, hmac-sha256, random, ed25519 */
static const NativeSymbol akira_crypto_natives[] = {
    {"crypto_sha256",            (void *)akira_native_crypto_sha256,            "(*~*)i",    NULL},
    {"crypto_aes256_encrypt",    (void *)akira_native_crypto_aes256_encrypt,    "(***~*)i",  NULL},
    {"crypto_aes256_decrypt",    (void *)akira_native_crypto_aes256_decrypt,    "(***~*)i",  NULL},
    {"crypto_hmac_sha256",       (void *)akira_native_crypto_hmac_sha256,       "(*~*~*)i",  NULL},
    {"crypto_random",            (void *)akira_native_crypto_random,            "(*~)i",     NULL},
    /* Ed25519 — seed_ptr(32B) and pub_ptr(32B) are fixed-size, validated inside */
    {"crypto_ed25519_keygen",    (void *)akira_native_crypto_ed25519_keygen,    "(**)i",     NULL},
    /* seed_ptr(32B), msg_ptr+msg_len pair (auto-validated), sig_ptr(64B) */
    {"crypto_ed25519_sign",      (void *)akira_native_crypto_ed25519_sign,      "(**~*)i",   NULL},
    /* key(32B), nonce(16B), in+in_len (validated pair), out (same len as in) */
    {"crypto_aes256_ctr",        (void *)akira_native_crypto_aes256_ctr,        "(***~*)i",  NULL},
    /* P-256 (U2F) — priv_ptr(32B), pub_ptr(65B) fixed-size, validated inside */
    {"crypto_p256_keygen",       (void *)akira_native_crypto_p256_keygen,       "(**)i",     NULL},
    /* priv_ptr(32B), msg_ptr+msg_len pair (auto-validated), sig_ptr(64B raw r||s) */
    {"crypto_p256_sign",         (void *)akira_native_crypto_p256_sign,         "(**~*)i",   NULL},
    /* SE050 hardware-backed P-256 (non-exportable) — slot(i), pub_ptr(65B).
     * Exported in every crypto build: without CONFIG_AKIRA_SE050_U2F the
     * stubs return -ENOTSUP, so an app can probe for the secure element
     * instead of trapping on an unlinked import. */
    {"crypto_p256_keygen_se050", (void *)akira_native_crypto_p256_keygen_se050, "(i*)i",     NULL},
    /* slot(i), hash_ptr(32B), sig_ptr(64B raw r||s) */
    {"crypto_p256_sign_se050",   (void *)akira_native_crypto_p256_sign_se050,   "(i**)i",    NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_crypto_api, "env", akira_crypto_natives);
#endif
