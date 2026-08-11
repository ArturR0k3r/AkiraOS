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

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif

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

/**
 * @brief AES-256-CTR encrypt/decrypt (CTR mode is its own inverse).
 * @param key_ptr    32-byte AES key.
 * @param nonce_ptr  16-byte counter/nonce block (initial counter value).
 * @param in_ptr     Input buffer (plaintext or ciphertext).
 * @param in_len     Input length in bytes (any length, no padding required).
 * @param out_ptr    Output buffer (same length as in_len).
 * @return 0 on success, negative errno on error.
 */
int akira_native_crypto_aes256_ctr(wasm_exec_env_t exec_env,
                                    void *key_ptr, void *nonce_ptr,
                                    void *in_ptr, uint32_t in_len,
                                    void *out_ptr);

/**
 * @brief Generate a new Ed25519 key pair from hardware entropy.
 *
 * Sources 32 bytes from sys_csrand_get() as the Ed25519 seed (private key
 * scalar), then derives the public key using Zephyr PSA Crypto.
 *
 * Gate: CONFIG_AKIRA_WASM_CRYPTO_ED25519=y (requires PSA_WANT_ALG_PURE_EDDSA).
 *
 * @param seed_ptr  WASM pointer to 32-byte output buffer (private seed).
 * @param pub_ptr   WASM pointer to 32-byte output buffer (public key).
 * @return 0 on success, -ENOTSUP if not compiled in, -EIO on PSA failure.
 */
int akira_native_crypto_ed25519_keygen(wasm_exec_env_t exec_env,
                                        void *seed_ptr,
                                        void *pub_ptr);

/**
 * @brief Sign a message with an Ed25519 private key seed (pure EdDSA).
 *
 * Imports the 32-byte seed into a volatile PSA key slot, signs the message,
 * then immediately destroys the slot. The seed is NOT stored on the host.
 *
 * @param seed_ptr  WASM pointer to 32-byte private key seed.
 * @param msg_ptr   WASM pointer to message buffer.
 * @param msg_len   Message length in bytes.
 * @param sig_ptr   WASM pointer to 64-byte signature output.
 * @return 0 on success, -ENOTSUP if not compiled in, -EIO on PSA failure.
 */
int akira_native_crypto_ed25519_sign(wasm_exec_env_t exec_env,
                                      void *seed_ptr,
                                      void *msg_ptr, uint32_t msg_len,
                                      void *sig_ptr);

/**
 * @brief Generate a new P-256 (secp256r1) key pair from hardware entropy.
 *
 * Unlike Ed25519, this goes through PSA properly (P-256 is a supported
 * Weierstrass curve in this mbedTLS build).
 *
 * Gate: CONFIG_AKIRA_WASM_CRYPTO_ECDSA_P256=y.
 *
 * @param priv_ptr  WASM pointer to 32-byte output buffer (private scalar).
 * @param pub_ptr   WASM pointer to 65-byte output buffer (uncompressed point).
 * @return 0 on success, -ENOTSUP if not compiled in, -EIO on PSA failure.
 */
int akira_native_crypto_p256_keygen(wasm_exec_env_t exec_env,
                                     void *priv_ptr,
                                     void *pub_ptr);

/**
 * @brief Sign a message with a P-256 private key (ECDSA/SHA-256).
 *
 * Imports the 32-byte private scalar into a volatile PSA key slot, signs
 * the message, then immediately destroys the slot. Output is raw r||s
 * (32+32 bytes), NOT DER — caller DER-encodes for CTAP2/U2F if needed.
 *
 * @param priv_ptr  WASM pointer to 32-byte private key scalar.
 * @param msg_ptr   WASM pointer to message buffer.
 * @param msg_len   Message length in bytes.
 * @param sig_ptr   WASM pointer to 64-byte signature output (raw r||s).
 * @return 0 on success, -ENOTSUP if not compiled in, -EIO on PSA failure.
 */
int akira_native_crypto_p256_sign(wasm_exec_env_t exec_env,
                                   void *priv_ptr,
                                   void *msg_ptr, uint32_t msg_len,
                                   void *sig_ptr);

/**
 * @brief Generate a hardware-backed P-256 key pair inside the SE050.
 *
 * The private key is generated on-die and is NON-EXPORTABLE — only the public
 * key is returned. Keys are addressed by @p slot (mapped to an SE050 object
 * ID); regenerating an existing slot is a no-op. For hardware-backed U2F/FIDO.
 *
 * Gate: CONFIG_AKIRA_SE050_U2F=y.
 *
 * @param slot     Key slot index (0..N); maps to SE050 object id base+slot.
 * @param pub_ptr  WASM pointer to 65-byte output buffer (uncompressed point).
 * @return 0 on success, -ENOTSUP if not compiled in, -ENODEV if no SE050.
 */
int akira_native_crypto_p256_keygen_se050(wasm_exec_env_t exec_env,
                                          uint32_t slot, void *pub_ptr);

/**
 * @brief ECDSA/SHA-256 sign a 32-byte digest with an SE050-held P-256 key.
 *
 * The private key never leaves the SE050. Output is raw r||s (64 bytes).
 *
 * Gate: CONFIG_AKIRA_SE050_U2F=y.
 *
 * @param slot      Key slot index used at keygen time.
 * @param hash_ptr  WASM pointer to the 32-byte message digest.
 * @param sig_ptr   WASM pointer to 64-byte signature output (raw r||s).
 * @return 0 on success, -ENOTSUP if not compiled in, -ENODEV if no SE050.
 */
int akira_native_crypto_p256_sign_se050(wasm_exec_env_t exec_env,
                                        uint32_t slot,
                                        void *hash_ptr, void *sig_ptr);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_CRYPTO_API_H */
