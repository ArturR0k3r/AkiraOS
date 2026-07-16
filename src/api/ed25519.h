/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ed25519.h
 * @brief Standalone Ed25519 (RFC 8032) keygen/sign.
 *
 * This mbedTLS vendor drop has no twisted-edwards curve in mbedtls_ecp,
 * so PSA_ECC_FAMILY_TWISTED_EDWARDS / PSA_ALG_PURE_EDDSA are unimplemented
 * spec constants only, and mbedtls_sha512() itself is unresolved at link
 * time on this target. This implements curve arithmetic on mbedtls_mpi
 * (bignum) plus a vendored standalone SHA-512, per RFC 8032 section 5.1.
 */

#ifndef AKIRA_ED25519_H
#define AKIRA_ED25519_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Derive an Ed25519 public key from a 32-byte seed.
 * @param seed[in]  32-byte private seed.
 * @param pub[out]  32-byte public key.
 * @return 0 on success, negative errno on failure.
 */
int ed25519_keygen(const uint8_t seed[32], uint8_t pub[32]);

/**
 * @brief Sign a message with an Ed25519 seed (RFC 8032 Sign).
 * @param seed[in]     32-byte private seed.
 * @param msg[in]      Message to sign.
 * @param msg_len[in]  Message length in bytes.
 * @param sig[out]     64-byte signature (R || S).
 * @return 0 on success, negative errno on failure.
 */
int ed25519_sign(const uint8_t seed[32], const uint8_t *msg, uint32_t msg_len,
                  uint8_t sig[64]);

/**
 * @brief Run RFC 8032 §7.1 test vector 1 against this implementation.
 * @return 0 if the implementation matches the known-answer vector.
 */
int ed25519_self_test(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_ED25519_H */
