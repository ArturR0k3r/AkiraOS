/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * HKDF stand-in so mesh_session.c links without the mesh crypto module; the
 * tests exercise the session table, not the key derivation.
 */

#include <string.h>
#include <errno.h>
#include "mesh/mesh_crypto.h"

int mesh_crypto_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
			    const uint8_t *salt, size_t salt_len,
			    const uint8_t *info, size_t info_len,
			    uint8_t *okm_out, size_t okm_len)
{
	(void)ikm; (void)ikm_len; (void)salt; (void)salt_len; (void)info; (void)info_len;
	memset(okm_out, 0, okm_len);
	return 0;
}
