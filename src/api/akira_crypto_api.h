/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_crypto_api.h
 * @brief Cryptographic operations WASM native API.
 *
 * Functions: sha256, aes256_cbc_encrypt, aes256_cbc_decrypt,
 *            hmac_sha256, random_bytes.
 *
 * Gate: CONFIG_AKIRA_WASM_CRYPTO=y (selects CONFIG_TINYCRYPT)
 * Capability: AKIRA_CAP_CRYPTO (bit 28)
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_CRYPTO_API_H
#define AKIRA_CRYPTO_API_H

#include <wasm_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SHA-256 hash of input data.
 * @param data_ptr   Pointer to input buffer in WASM linear memory.
 * @param data_len   Input length in bytes.
 * @param out_ptr    Pointer to 32-byte output buffer in WASM linear memory.
 * @return 0 on success, negative errno on error.
 */
int akira_native_crypto_sha256(wasm_exec_env_t exec_env,
                                void *data_ptr, uint32_t data_len,
                                void *out_ptr);

/**
 * @brief AES-256-CBC encrypt.
 * @param key_ptr    32-byte key.
 * @param iv_ptr     16-byte IV.
 * @param in_ptr     Plaintext buffer (must be multiple of 16 bytes).
 * @param in_len     Plaintext length.
 * @param out_ptr    Ciphertext output buffer (same size as in_len).
 * @return 0 on success, negative errno on error.
 */
int akira_native_crypto_aes256_encrypt(wasm_exec_env_t exec_env,
                                        void *key_ptr, void *iv_ptr,
                                        void *in_ptr, uint32_t in_len,
                                        void *out_ptr);

/**
 * @brief AES-256-CBC decrypt.
 * Same layout as encrypt.
 */
int akira_native_crypto_aes256_decrypt(wasm_exec_env_t exec_env,
                                        void *key_ptr, void *iv_ptr,
                                        void *in_ptr, uint32_t in_len,
                                        void *out_ptr);

/**
 * @brief HMAC-SHA256.
 * @param key_ptr    Key buffer.
 * @param key_len    Key length.
 * @param data_ptr   Data buffer.
 * @param data_len   Data length.
 * @param out_ptr    32-byte HMAC output buffer.
 * @return 0 on success, negative errno on error.
 */
int akira_native_crypto_hmac_sha256(wasm_exec_env_t exec_env,
                                     void *key_ptr, uint32_t key_len,
                                     void *data_ptr, uint32_t data_len,
                                     void *out_ptr);

/**
 * @brief Fill buffer with cryptographically secure random bytes.
 * @param buf_ptr    Output buffer in WASM linear memory.
 * @param len        Number of bytes to generate.
 * @return 0 on success, negative errno on error.
 */
int akira_native_crypto_random(wasm_exec_env_t exec_env,
                                void *buf_ptr, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CRYPTO_API_H */
