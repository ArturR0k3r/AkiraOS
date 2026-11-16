/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wrapper over the vendored fiat-crypto Ed25519 verifier (curve25519.c).
 */

#include <akira_ed25519.h>

#include <errno.h>
#include <zephyr/sys/util.h>

int akira_fiat_ed25519_verify(const uint8_t *message, size_t message_len,
                              const uint8_t signature[64],
                              const uint8_t public_key[32]);

int akira_ed25519_verify(const uint8_t *msg, size_t len,
                         const uint8_t sig[AKIRA_ED25519_SIG_LEN],
                         const uint8_t pubkey[AKIRA_ED25519_PUBKEY_LEN])
{
	static const uint8_t empty;

	if (!sig || !pubkey || (!msg && len != 0)) {
		return -EINVAL;
	}
	return akira_fiat_ed25519_verify(msg ? msg : &empty, len, sig, pubkey) == 1
		       ? 0
		       : -EBADMSG;
}

int akira_ed25519_pubkey_from_hex(const char *hex,
				  uint8_t out[AKIRA_ED25519_PUBKEY_LEN])
{
	if (!hex || !out) {
		return -EINVAL;
	}
	for (size_t i = 0; i < AKIRA_ED25519_PUBKEY_LEN * 2; i++) {
		if (hex[i] == '\0') {
			return -EINVAL;
		}
	}
	if (hex[AKIRA_ED25519_PUBKEY_LEN * 2] != '\0') {
		return -EINVAL;
	}
	return hex2bin(hex, AKIRA_ED25519_PUBKEY_LEN * 2, out,
		       AKIRA_ED25519_PUBKEY_LEN) == AKIRA_ED25519_PUBKEY_LEN
		       ? 0
		       : -EINVAL;
}
