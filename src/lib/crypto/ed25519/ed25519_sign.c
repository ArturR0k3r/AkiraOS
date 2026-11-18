/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ed25519 key derivation and signing (RFC 8032 section 5.1.5 and 5.1.6)
 * over the vendored fiat-crypto field and group code (curve25519.c), which
 * MCUboot uses only to verify.
 *
 * Secret values only meet constant-time code: the base-point
 * multiplication always doubles, always adds and picks the result with a
 * mask, and the scalar multiply-add is fixed-length schoolbook arithmetic
 * followed by the vendored constant-time reduction mod L.
 */

#include "ed25519_sign.h"

#include <errno.h>
#include <string.h>

#include <mbedtls/platform_util.h>
#include <mbedtls/sha512.h>

/* curve25519.h defines the fiat helpers as static functions; this file
 * only needs its types. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "curve25519.h"
#pragma GCC diagnostic pop

/* Exported by curve25519.c under these names (see its rename block). */
int akira_fiat_ge_frombytes_vartime(ge_p3 *h, const uint8_t s[32]);
void akira_fiat_ge_tobytes(uint8_t s[32], const ge_p2 *h);
void akira_fiat_ge_p3_to_cached(ge_cached *r, const ge_p3 *p);
void akira_fiat_ge_p1p1_to_p3(ge_p3 *r, const ge_p1p1 *p);
void akira_fiat_ge_add(ge_p1p1 *r, const ge_p3 *p, const ge_cached *q);
void akira_fiat_sc_reduce(uint8_t s[64]);

/* The base point B, compressed: y = 4/5, x even. */
static const uint8_t k_base[32] = {
	0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
};

static uint32_t load_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* out = SHA-512(a || b || c); a part with length 0 is skipped. */
static int sha512_3(uint8_t out[64], const uint8_t *a, size_t alen,
		    const uint8_t *b, size_t blen, const uint8_t *c, size_t clen)
{
	mbedtls_sha512_context ctx;
	int ret;

	mbedtls_sha512_init(&ctx);
	ret = mbedtls_sha512_starts(&ctx, 0);
	if (ret == 0 && alen) {
		ret = mbedtls_sha512_update(&ctx, a, alen);
	}
	if (ret == 0 && blen) {
		ret = mbedtls_sha512_update(&ctx, b, blen);
	}
	if (ret == 0 && clen) {
		ret = mbedtls_sha512_update(&ctx, c, clen);
	}
	if (ret == 0) {
		ret = mbedtls_sha512_finish(&ctx, out);
	}
	mbedtls_sha512_free(&ctx);
	return ret == 0 ? 0 : -EIO;
}

/* r = bit ? a : r, without branching on bit. */
static void ge_p3_select(ge_p3 *r, const ge_p3 *a, uint32_t bit)
{
	const uint32_t mask = value_barrier_u32(0u - bit);
	fe *rf[4] = { &r->X, &r->Y, &r->Z, &r->T };
	const fe *af[4] = { &a->X, &a->Y, &a->Z, &a->T };

	for (int k = 0; k < 4; k++) {
		for (int i = 0; i < 10; i++) {
			rf[k]->v[i] ^= mask & (rf[k]->v[i] ^ af[k]->v[i]);
		}
	}
}

/* r = scalar * B. The group addition is complete on edwards25519, so it
 * also doubles and handles the identity. */
static void scalarmult_base(ge_p3 *r, const uint8_t scalar[32])
{
	ge_p3 base;
	ge_p3 sum3;
	ge_cached cached;
	ge_cached base_cached;
	ge_p1p1 sum;

	/* Decoding the fixed base point cannot fail. */
	(void)akira_fiat_ge_frombytes_vartime(&base, k_base);
	akira_fiat_ge_p3_to_cached(&base_cached, &base);

	memset(r, 0, sizeof(*r)); /* identity: (0 : 1 : 1 : 0) */
	r->Y.v[0] = 1;
	r->Z.v[0] = 1;

	for (int i = 255; i >= 0; i--) {
		uint32_t bit = (scalar[i >> 3] >> (i & 7)) & 1u;

		akira_fiat_ge_p3_to_cached(&cached, r);
		akira_fiat_ge_add(&sum, r, &cached);
		akira_fiat_ge_p1p1_to_p3(r, &sum);
		akira_fiat_ge_add(&sum, r, &base_cached);
		akira_fiat_ge_p1p1_to_p3(&sum3, &sum);
		ge_p3_select(r, &sum3, bit);
	}

	mbedtls_platform_zeroize(&sum3, sizeof(sum3));
	mbedtls_platform_zeroize(&cached, sizeof(cached));
	mbedtls_platform_zeroize(&sum, sizeof(sum));
}

static void ge_p3_encode(uint8_t out[32], const ge_p3 *p)
{
	ge_p2 p2;

	memcpy(&p2.X, &p->X, sizeof(p2.X));
	memcpy(&p2.Y, &p->Y, sizeof(p2.Y));
	memcpy(&p2.Z, &p->Z, sizeof(p2.Z));
	akira_fiat_ge_tobytes(out, &p2);
	mbedtls_platform_zeroize(&p2, sizeof(p2));
}

/* s = (a * b + c) mod L, all 32-byte little-endian. a * b + c < 2^512
 * for every caller (a < 2^255, b and c < L), so the 64-byte sum is exact
 * before the reduction. */
static void sc_muladd(uint8_t s[32], const uint8_t a[32], const uint8_t b[32],
		      const uint8_t c[32])
{
	uint32_t al[8];
	uint32_t bl[8];
	uint32_t prod[16] = { 0 };
	uint8_t wide[64];
	uint64_t carry;

	for (int i = 0; i < 8; i++) {
		al[i] = load_le32(a + 4 * i);
		bl[i] = load_le32(b + 4 * i);
	}
	for (int i = 0; i < 8; i++) {
		carry = 0;
		for (int j = 0; j < 8; j++) {
			uint64_t t = (uint64_t)al[i] * bl[j] + prod[i + j] + carry;

			prod[i + j] = (uint32_t)t;
			carry = t >> 32;
		}
		prod[i + 8] = (uint32_t)carry;
	}
	carry = 0;
	for (int i = 0; i < 16; i++) {
		uint64_t t = (uint64_t)prod[i] + carry;

		if (i < 8) {
			t += load_le32(c + 4 * i);
		}
		prod[i] = (uint32_t)t;
		carry = t >> 32;
	}
	for (int i = 0; i < 16; i++) {
		store_le32(wide + 4 * i, prod[i]);
	}
	akira_fiat_sc_reduce(wide);
	memcpy(s, wide, 32);

	mbedtls_platform_zeroize(al, sizeof(al));
	mbedtls_platform_zeroize(prod, sizeof(prod));
	mbedtls_platform_zeroize(wide, sizeof(wide));
}

static void clamp(uint8_t a[32])
{
	a[0] &= 248;
	a[31] &= 127;
	a[31] |= 64;
}

int akira_ed25519_pubkey_from_seed(const uint8_t seed[32], uint8_t pub[32])
{
	uint8_t az[64];
	ge_p3 point;
	int ret;

	if (!seed || !pub) {
		return -EINVAL;
	}
	ret = sha512_3(az, seed, 32, NULL, 0, NULL, 0);
	if (ret == 0) {
		clamp(az);
		scalarmult_base(&point, az);
		ge_p3_encode(pub, &point);
	}
	mbedtls_platform_zeroize(az, sizeof(az));
	mbedtls_platform_zeroize(&point, sizeof(point));
	return ret;
}

int akira_ed25519_sign(const uint8_t *msg, size_t len, const uint8_t seed[32],
		       uint8_t sig[64])
{
	uint8_t az[64];    /* SHA-512(seed): secret scalar a || prefix */
	uint8_t nonce[64]; /* r */
	uint8_t hram[64];  /* k = SHA-512(R || A || M) */
	uint8_t pub[32];
	ge_p3 point;
	int ret;

	if (!seed || !sig || (!msg && len != 0)) {
		return -EINVAL;
	}

	ret = sha512_3(az, seed, 32, NULL, 0, NULL, 0);
	if (ret == 0) {
		clamp(az);
		scalarmult_base(&point, az);
		ge_p3_encode(pub, &point);
		ret = sha512_3(nonce, az + 32, 32, msg, len, NULL, 0);
	}
	if (ret == 0) {
		akira_fiat_sc_reduce(nonce);
		scalarmult_base(&point, nonce);
		ge_p3_encode(sig, &point); /* R */
		ret = sha512_3(hram, sig, 32, pub, 32, msg, len);
	}
	if (ret == 0) {
		akira_fiat_sc_reduce(hram);
		sc_muladd(sig + 32, hram, az, nonce); /* S = r + k * a */
	} else {
		mbedtls_platform_zeroize(sig, 64);
	}

	mbedtls_platform_zeroize(az, sizeof(az));
	mbedtls_platform_zeroize(nonce, sizeof(nonce));
	mbedtls_platform_zeroize(hram, sizeof(hram));
	mbedtls_platform_zeroize(&point, sizeof(point));
	return ret;
}
