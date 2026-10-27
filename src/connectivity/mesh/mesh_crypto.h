/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file mesh_crypto.h
 * @brief P-256 ECDH + AES-256-CTR/HMAC-SHA256 primitives for AkiraMesh E2E.
 *
 * Plain Zephyr C, not the WASM-sandboxed API in akira_crypto_api.c (that
 * surface requires a wasm_exec_env_t and WASM linear-memory validation and
 * cannot be called from core firmware code). Same PSA/mbedTLS backend,
 * same P-256 curve — the only curve confirmed working through PSA on this
 * hardware (see akira_crypto_api.c's Ed25519 PSA_ERROR_NOT_SUPPORTED note).
 *
 * Gate: CONFIG_AKIRA_MESH_E2E_CRYPTO=y.
 */

#ifndef AKIRA_MESH_CRYPTO_H
#define AKIRA_MESH_CRYPTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_CRYPTO_PRIV_LEN        32  /* P-256 private scalar */
#define MESH_CRYPTO_PUB_LEN         65  /* P-256 uncompressed point (0x04||X||Y) */
#define MESH_CRYPTO_SHARED_LEN      32  /* ECDH raw shared secret (X-coordinate) */
#define MESH_CRYPTO_SESSION_KEY_LEN 32  /* AES-256-CTR key */
#define MESH_CRYPTO_MAC_KEY_LEN     32  /* HMAC-SHA256 key */
#define MESH_CRYPTO_NONCE_LEN       16  /* AES-CTR initial counter block */
#define MESH_CRYPTO_MAC_LEN         16  /* truncated HMAC tag carried on the wire */
#define MESH_CRYPTO_OVERHEAD        (MESH_CRYPTO_NONCE_LEN + MESH_CRYPTO_MAC_LEN)

/**
 * @brief Generate a new P-256 key pair from hardware entropy via PSA.
 * @return 0 on success, -ENOTSUP if not compiled in, -EIO on PSA failure.
 */
int mesh_crypto_p256_keygen(uint8_t priv_out[MESH_CRYPTO_PRIV_LEN],
                            uint8_t pub_out[MESH_CRYPTO_PUB_LEN]);

/**
 * @brief Raw ECDH(priv, peer_pub) over P-256 — returns the shared X-coordinate.
 * @return 0 on success, -ENOTSUP if not compiled in, -EIO on PSA failure.
 */
int mesh_crypto_p256_ecdh(const uint8_t priv[MESH_CRYPTO_PRIV_LEN],
                          const uint8_t peer_pub[MESH_CRYPTO_PUB_LEN],
                          uint8_t shared_out[MESH_CRYPTO_SHARED_LEN]);

/**
 * @brief HMAC-SHA256(key, data). Hand-rolled on mbedtls_sha256 (same
 * approach as akira_crypto_api.c's hmac_sha256 — avoids a MBEDTLS_MD_C dep).
 * @param key_len Must be <= 64 (SHA-256 block size).
 */
void mesh_crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                             const uint8_t *data, size_t data_len,
                             uint8_t out[32]);

/**
 * @brief HKDF-SHA256 (RFC 5869) Extract-then-Expand, built on
 * mesh_crypto_hmac_sha256. Only supports okm_len <= 64 (two expand blocks)
 * and info_len <= 64 — enough for a single 64-byte (enc_key||mac_key)
 * derivation, the only use case here.
 * @return 0 on success, -EINVAL if okm_len or info_len exceed the limits above.
 */
int mesh_crypto_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                            const uint8_t *salt, size_t salt_len,
                            const uint8_t *info, size_t info_len,
                            uint8_t *okm_out, size_t okm_len);

/**
 * @brief AES-256-CTR encrypt/decrypt (CTR is its own inverse).
 * @return 0 on success, -EIO on mbedTLS failure.
 */
int mesh_crypto_aes256_ctr(const uint8_t key[32], const uint8_t nonce[16],
                           const uint8_t *in, size_t len, uint8_t *out);

/**
 * @brief Constant-time byte-buffer comparison (MAC verification).
 * @return true if equal.
 */
bool mesh_crypto_const_time_eq(const uint8_t *a, const uint8_t *b, size_t len);

/**
 * @brief Load this node's long-term P-256 identity keypair from settings,
 * generating and persisting a new one on first boot.
 *
 * Storage keys: "mesh_id_priv" (hex, encrypted-at-rest) and "mesh_id_pub"
 * (hex, plaintext — not secret). Requires AKIRA_SETTINGS_MAX_VALUE_LEN >=
 * 131 bytes (see prj.conf) to fit the hex-encoded 65-byte pubkey.
 *
 * @return 0 on success, -EIO on settings or PSA failure.
 */
int mesh_crypto_identity_init(uint8_t priv_out[MESH_CRYPTO_PRIV_LEN],
                              uint8_t pub_out[MESH_CRYPTO_PUB_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MESH_CRYPTO_H */
