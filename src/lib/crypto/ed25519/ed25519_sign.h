/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ed25519 (RFC 8032, PureEdDSA) key derivation and signing, internal to
 * AkiraOS (the WASM crypto API uses it). Built on the vendored fiat-crypto
 * field and group code in curve25519.c.
 */

#ifndef AKIRA_ED25519_SIGN_H
#define AKIRA_ED25519_SIGN_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Derive the public key of a 32-byte Ed25519 seed (private key).
 *
 * @return 0 on success, -EINVAL on a NULL argument, -EIO if hashing failed.
 */
int akira_ed25519_pubkey_from_seed(const uint8_t seed[32], uint8_t pub[32]);

/**
 * @brief Sign a message with a 32-byte Ed25519 seed (private key).
 *
 * Deterministic: the same seed and message always give the same signature.
 * @p msg is read twice, so it must not change during the call. @p sig may
 * not overlap @p msg or @p seed.
 *
 * @return 0 on success, -EINVAL on a NULL argument, -EIO if hashing failed.
 */
int akira_ed25519_sign(const uint8_t *msg, size_t len, const uint8_t seed[32],
		       uint8_t sig[64]);

#endif /* AKIRA_ED25519_SIGN_H */
