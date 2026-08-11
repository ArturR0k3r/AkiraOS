/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ed25519.c
 * @brief Standalone Ed25519 (RFC 8032) on top of mbedtls_mpi + a vendored SHA-512.
 *
 * Curve arithmetic uses extended twisted-Edwards coordinates with the
 * complete addition/doubling formulas from Hisil-Wong-Carter-Dawson
 * ("add-2008-hwcd-3" / "dbl-2008-hwcd") specialised for a = -1. Only
 * point encode (not decode) is needed since this module only signs.
 */

#include "ed25519.h"

#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <mbedtls/bignum.h>

/*
 * ---- SHA-512 (FIPS 180-4) ----------------------------------------------
 * Vendored standalone: on this target, mbedtls_sha512() is unresolved at
 * link time (ESP32 mbedTLS port ALT-guards library/sha512.c without
 * wiring a hardware SHA-512 implementation for this Zephyr integration),
 * so PSA/mbedTLS's SHA-512 cannot be relied on here. ed25519_self_test()
 * validates this against a known RFC 8032 answer at boot.
 */

static const uint64_t sha512_iv[8] = {
	0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
	0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
	0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
	0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

static const uint64_t sha512_k[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
	0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
	0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
	0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static inline uint64_t sha512_rotr(uint64_t x, unsigned n)
{
	return (x >> n) | (x << (64 - n));
}

static void sha512_block(uint64_t H[8], const uint8_t block[128])
{
	uint64_t w[80];
	uint64_t a, b, c, d, e, f, g, h;

	for (int t = 0; t < 16; t++) {
		w[t] = ((uint64_t)block[t * 8 + 0] << 56) |
		       ((uint64_t)block[t * 8 + 1] << 48) |
		       ((uint64_t)block[t * 8 + 2] << 40) |
		       ((uint64_t)block[t * 8 + 3] << 32) |
		       ((uint64_t)block[t * 8 + 4] << 24) |
		       ((uint64_t)block[t * 8 + 5] << 16) |
		       ((uint64_t)block[t * 8 + 6] << 8) |
		       ((uint64_t)block[t * 8 + 7]);
	}
	for (int t = 16; t < 80; t++) {
		uint64_t s0 = sha512_rotr(w[t - 15], 1) ^ sha512_rotr(w[t - 15], 8) ^ (w[t - 15] >> 7);
		uint64_t s1 = sha512_rotr(w[t - 2], 19) ^ sha512_rotr(w[t - 2], 61) ^ (w[t - 2] >> 6);
		w[t] = w[t - 16] + s0 + w[t - 7] + s1;
	}

	a = H[0]; b = H[1]; c = H[2]; d = H[3];
	e = H[4]; f = H[5]; g = H[6]; h = H[7];

	for (int t = 0; t < 80; t++) {
		uint64_t S1 = sha512_rotr(e, 14) ^ sha512_rotr(e, 18) ^ sha512_rotr(e, 41);
		uint64_t ch = (e & f) ^ (~e & g);
		uint64_t t1 = h + S1 + ch + sha512_k[t] + w[t];
		uint64_t S0 = sha512_rotr(a, 28) ^ sha512_rotr(a, 34) ^ sha512_rotr(a, 39);
		uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint64_t t2 = S0 + maj;

		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	H[0] += a; H[1] += b; H[2] += c; H[3] += d;
	H[4] += e; H[5] += f; H[6] += g; H[7] += h;
}

static int sha512_oneshot(const uint8_t *msg, size_t len, uint8_t out[64])
{
	uint64_t H[8];
	size_t padded_len = ((len + 1 + 16 + 127) / 128) * 128;
	uint8_t *buf = k_malloc(padded_len);
	if (!buf) return -1;

	memcpy(H, sha512_iv, sizeof(H));
	memset(buf, 0, padded_len);
	memcpy(buf, msg, len);
	buf[len] = 0x80u;

	uint64_t bitlen = (uint64_t)len * 8u;
	for (int i = 0; i < 8; i++) {
		buf[padded_len - 1 - i] = (uint8_t)(bitlen >> (8 * i));
	}

	for (size_t off = 0; off < padded_len; off += 128) {
		sha512_block(H, buf + off);
	}

	for (int i = 0; i < 8; i++) {
		out[i * 8 + 0] = (uint8_t)(H[i] >> 56);
		out[i * 8 + 1] = (uint8_t)(H[i] >> 48);
		out[i * 8 + 2] = (uint8_t)(H[i] >> 40);
		out[i * 8 + 3] = (uint8_t)(H[i] >> 32);
		out[i * 8 + 4] = (uint8_t)(H[i] >> 24);
		out[i * 8 + 5] = (uint8_t)(H[i] >> 16);
		out[i * 8 + 6] = (uint8_t)(H[i] >> 8);
		out[i * 8 + 7] = (uint8_t)(H[i]);
	}

	memset(buf, 0, padded_len);
	k_free(buf);
	return 0;
}

/* p = 2^255 - 19 */
static const char *P_STR =
    "57896044618658097711785492504343953926634992332820282019728792003956564819949";
/* d = -121665/121666 mod p */
static const char *D_STR =
    "37095705934669439343138083508754565189542113879843219016388785533085940283555";
/* L = order of the base point = 2^252 + 27742317777372353535851937790883648493 */
static const char *L_STR =
    "7237005577332262213973186563042994240857116359379907606001950938285454250989";
static const char *BX_STR =
    "15112221349535400772501151409588531511454012693041857206046113283949847762202";
static const char *BY_STR =
    "46316835694926478169428394003475163141307993866256225615783033603165251855960";
/* sqrt(-1) mod p = 2^((p-1)/4) mod p (RFC 8032 §5.1.3 point decoding) */
static const char *SQRT_M1_STR =
    "19681161376707505956807079304988542015446066515923890162744021073123829784752";

typedef struct {
	mbedtls_mpi X, Y, Z, T;
} ge25519;

static void ge_init(ge25519 *p)
{
	mbedtls_mpi_init(&p->X);
	mbedtls_mpi_init(&p->Y);
	mbedtls_mpi_init(&p->Z);
	mbedtls_mpi_init(&p->T);
}

static void ge_free(ge25519 *p)
{
	mbedtls_mpi_free(&p->X);
	mbedtls_mpi_free(&p->Y);
	mbedtls_mpi_free(&p->Z);
	mbedtls_mpi_free(&p->T);
}

/* r = a mod p, kept in canonical non-negative form */
static int fe_reduce(mbedtls_mpi *r, const mbedtls_mpi *a, const mbedtls_mpi *p)
{
	return mbedtls_mpi_mod_mpi(r, a, p);
}

static int fe_mul(mbedtls_mpi *r, const mbedtls_mpi *a, const mbedtls_mpi *b,
                   const mbedtls_mpi *p)
{
	mbedtls_mpi t;
	mbedtls_mpi_init(&t);
	int ret = mbedtls_mpi_mul_mpi(&t, a, b);
	if (ret == 0) {
		ret = fe_reduce(r, &t, p);
	}
	mbedtls_mpi_free(&t);
	return ret;
}

static int fe_add(mbedtls_mpi *r, const mbedtls_mpi *a, const mbedtls_mpi *b,
                   const mbedtls_mpi *p)
{
	mbedtls_mpi t;
	mbedtls_mpi_init(&t);
	int ret = mbedtls_mpi_add_mpi(&t, a, b);
	if (ret == 0) {
		ret = fe_reduce(r, &t, p);
	}
	mbedtls_mpi_free(&t);
	return ret;
}

static int fe_sub(mbedtls_mpi *r, const mbedtls_mpi *a, const mbedtls_mpi *b,
                   const mbedtls_mpi *p)
{
	mbedtls_mpi t;
	mbedtls_mpi_init(&t);
	int ret = mbedtls_mpi_sub_mpi(&t, a, b);
	if (ret == 0) {
		ret = fe_reduce(r, &t, p);
	}
	mbedtls_mpi_free(&t);
	return ret;
}

static int fe_inv(mbedtls_mpi *r, const mbedtls_mpi *a, const mbedtls_mpi *p)
{
	mbedtls_mpi pm2;
	mbedtls_mpi_init(&pm2);
	int ret = mbedtls_mpi_sub_int(&pm2, p, 2);
	if (ret == 0) {
		ret = mbedtls_mpi_exp_mod(r, a, &pm2, p, NULL);
	}
	mbedtls_mpi_free(&pm2);
	return ret;
}

/*
 * out = 2*in (mod p): canonical "dbl-2008-hwcd" formula for a = -1
 * twisted-edwards extended coordinates:
 *   A = X1^2 ; B = Y1^2 ; C = 2*Z1^2 ; D = -A
 *   E = (X1+Y1)^2 - A - B ; G = D+B ; F = G-C ; H = D-B
 *   X3 = E*F ; Y3 = G*H ; T3 = E*H ; Z3 = F*G
 */
static int point_double(ge25519 *out, const ge25519 *in, const mbedtls_mpi *p)
{
	mbedtls_mpi A, B, C, D, E, G, F, H, xy, xy2;
	int ret;

	mbedtls_mpi_init(&A); mbedtls_mpi_init(&B); mbedtls_mpi_init(&C);
	mbedtls_mpi_init(&D); mbedtls_mpi_init(&E); mbedtls_mpi_init(&G);
	mbedtls_mpi_init(&F); mbedtls_mpi_init(&H);
	mbedtls_mpi_init(&xy); mbedtls_mpi_init(&xy2);

	ret = fe_mul(&A, &in->X, &in->X, p);
	if (ret) goto done;
	ret = fe_mul(&B, &in->Y, &in->Y, p);
	if (ret) goto done;
	ret = fe_mul(&C, &in->Z, &in->Z, p);
	if (ret) goto done;
	ret = fe_add(&C, &C, &C, p);
	if (ret) goto done;

	ret = mbedtls_mpi_sub_mpi(&D, p, &A);                /* D = -A mod p */
	if (ret) goto done;
	ret = fe_reduce(&D, &D, p);
	if (ret) goto done;

	ret = fe_add(&xy, &in->X, &in->Y, p);
	if (ret) goto done;
	ret = fe_mul(&xy2, &xy, &xy, p);
	if (ret) goto done;
	ret = fe_sub(&E, &xy2, &A, p);
	if (ret) goto done;
	ret = fe_sub(&E, &E, &B, p);
	if (ret) goto done;

	ret = fe_add(&G, &D, &B, p);
	if (ret) goto done;
	ret = fe_sub(&F, &G, &C, p);
	if (ret) goto done;
	ret = fe_sub(&H, &D, &B, p);
	if (ret) goto done;

	ret = fe_mul(&out->X, &E, &F, p);
	if (ret) goto done;
	ret = fe_mul(&out->Y, &G, &H, p);
	if (ret) goto done;
	ret = fe_mul(&out->T, &E, &H, p);
	if (ret) goto done;
	ret = fe_mul(&out->Z, &F, &G, p);

done:
	mbedtls_mpi_free(&A); mbedtls_mpi_free(&B); mbedtls_mpi_free(&C);
	mbedtls_mpi_free(&D); mbedtls_mpi_free(&E); mbedtls_mpi_free(&G);
	mbedtls_mpi_free(&F); mbedtls_mpi_free(&H);
	mbedtls_mpi_free(&xy); mbedtls_mpi_free(&xy2);
	return ret;
}

/* out = a+b (mod p), complete addition formula (add-2008-hwcd-3, a=-1) */
static int point_add(ge25519 *out, const ge25519 *a, const ge25519 *b,
                      const mbedtls_mpi *p, const mbedtls_mpi *d2)
{
	mbedtls_mpi A, B, C, Dd, E, F, G, H, t1, t2;
	int ret;

	mbedtls_mpi_init(&A); mbedtls_mpi_init(&B); mbedtls_mpi_init(&C);
	mbedtls_mpi_init(&Dd); mbedtls_mpi_init(&E); mbedtls_mpi_init(&F);
	mbedtls_mpi_init(&G); mbedtls_mpi_init(&H);
	mbedtls_mpi_init(&t1); mbedtls_mpi_init(&t2);

	ret = fe_sub(&t1, &a->Y, &a->X, p);
	if (ret) goto done;
	ret = fe_sub(&t2, &b->Y, &b->X, p);
	if (ret) goto done;
	ret = fe_mul(&A, &t1, &t2, p);                       /* A = (Y1-X1)(Y2-X2) */
	if (ret) goto done;

	ret = fe_add(&t1, &a->Y, &a->X, p);
	if (ret) goto done;
	ret = fe_add(&t2, &b->Y, &b->X, p);
	if (ret) goto done;
	ret = fe_mul(&B, &t1, &t2, p);                       /* B = (Y1+X1)(Y2+X2) */
	if (ret) goto done;

	ret = fe_mul(&t1, &a->T, &b->T, p);
	if (ret) goto done;
	ret = fe_mul(&C, &t1, d2, p);                        /* C = T1*2d*T2 */
	if (ret) goto done;

	ret = fe_mul(&t1, &a->Z, &b->Z, p);
	if (ret) goto done;
	ret = fe_add(&Dd, &t1, &t1, p);                      /* D = 2*Z1*Z2 */
	if (ret) goto done;

	ret = fe_sub(&E, &B, &A, p);
	if (ret) goto done;
	ret = fe_sub(&F, &Dd, &C, p);
	if (ret) goto done;
	ret = fe_add(&G, &Dd, &C, p);
	if (ret) goto done;
	ret = fe_add(&H, &B, &A, p);
	if (ret) goto done;

	ret = fe_mul(&out->X, &E, &F, p);
	if (ret) goto done;
	ret = fe_mul(&out->Y, &G, &H, p);
	if (ret) goto done;
	ret = fe_mul(&out->T, &E, &H, p);
	if (ret) goto done;
	ret = fe_mul(&out->Z, &F, &G, p);

done:
	mbedtls_mpi_free(&A); mbedtls_mpi_free(&B); mbedtls_mpi_free(&C);
	mbedtls_mpi_free(&Dd); mbedtls_mpi_free(&E); mbedtls_mpi_free(&F);
	mbedtls_mpi_free(&G); mbedtls_mpi_free(&H);
	mbedtls_mpi_free(&t1); mbedtls_mpi_free(&t2);
	return ret;
}

/* out = scalar * base, via MSB-to-LSB double-and-add */
static int scalarmult(ge25519 *out, const mbedtls_mpi *scalar, const ge25519 *base,
                       const mbedtls_mpi *p, const mbedtls_mpi *d2)
{
	int ret;
	ge25519 acc;
	size_t nbits = mbedtls_mpi_bitlen(scalar);

	ge_init(&acc);
	ret = mbedtls_mpi_lset(&acc.X, 0);
	if (ret) goto done;
	ret = mbedtls_mpi_lset(&acc.Y, 1);
	if (ret) goto done;
	ret = mbedtls_mpi_lset(&acc.Z, 1);
	if (ret) goto done;
	ret = mbedtls_mpi_lset(&acc.T, 0);
	if (ret) goto done;

	for (size_t i = nbits; i-- > 0;) {
		ge25519 dbl;
		ge_init(&dbl);
		ret = point_double(&dbl, &acc, p);
		if (ret) { ge_free(&dbl); goto done; }
		ge_free(&acc);
		acc = dbl;

		int bit = mbedtls_mpi_get_bit(scalar, i);
		if (bit < 0) { ret = bit; goto done; }
		if (bit) {
			ge25519 sum;
			ge_init(&sum);
			ret = point_add(&sum, &acc, base, p, d2);
			if (ret) { ge_free(&sum); goto done; }
			ge_free(&acc);
			acc = sum;
		}
	}

	ge_init(out);
	ret = mbedtls_mpi_copy(&out->X, &acc.X);
	if (ret) goto done;
	ret = mbedtls_mpi_copy(&out->Y, &acc.Y);
	if (ret) goto done;
	ret = mbedtls_mpi_copy(&out->Z, &acc.Z);
	if (ret) goto done;
	ret = mbedtls_mpi_copy(&out->T, &acc.T);

done:
	ge_free(&acc);
	return ret;
}

static int point_encode(const ge25519 *pt, const mbedtls_mpi *p, uint8_t out[32])
{
	mbedtls_mpi zinv, x, y;
	int ret;

	mbedtls_mpi_init(&zinv); mbedtls_mpi_init(&x); mbedtls_mpi_init(&y);

	ret = fe_inv(&zinv, &pt->Z, p);
	if (ret) goto done;
	ret = fe_mul(&x, &pt->X, &zinv, p);
	if (ret) goto done;
	ret = fe_mul(&y, &pt->Y, &zinv, p);
	if (ret) goto done;

	ret = mbedtls_mpi_write_binary_le(&y, out, 32);
	if (ret) goto done;

	int x0 = mbedtls_mpi_get_bit(&x, 0);
	if (x0 < 0) { ret = x0; goto done; }
	if (x0) {
		out[31] |= 0x80u;
	} else {
		out[31] &= 0x7Fu;
	}

done:
	mbedtls_mpi_free(&zinv); mbedtls_mpi_free(&x); mbedtls_mpi_free(&y);
	return ret;
}

/* scalar = little-endian 64-byte hash, reduced mod L */
static int scalar_from_hash_le(mbedtls_mpi *scalar, const uint8_t h[64],
                                const mbedtls_mpi *L)
{
	int ret = mbedtls_mpi_read_binary_le(scalar, h, 64);
	if (ret) return ret;
	return mbedtls_mpi_mod_mpi(scalar, scalar, L);
}

static void ed25519_clamp(uint8_t a[32])
{
	a[0] &= 248u;
	a[31] &= 127u;
	a[31] |= 64u;
}

struct curve_ctx {
	mbedtls_mpi p, d, d2, L, sqrt_m1;
	ge25519 B;
};

static int curve_ctx_init(struct curve_ctx *c)
{
	int ret;

	mbedtls_mpi_init(&c->p);
	mbedtls_mpi_init(&c->d);
	mbedtls_mpi_init(&c->d2);
	mbedtls_mpi_init(&c->L);
	mbedtls_mpi_init(&c->sqrt_m1);
	ge_init(&c->B);

	ret = mbedtls_mpi_read_string(&c->p, 10, P_STR);
	if (ret) return ret;
	ret = mbedtls_mpi_read_string(&c->L, 10, L_STR);
	if (ret) return ret;
	ret = mbedtls_mpi_read_string(&c->sqrt_m1, 10, SQRT_M1_STR);
	if (ret) return ret;

	ret = mbedtls_mpi_read_string(&c->d, 10, D_STR);
	if (ret) return ret;
	ret = fe_add(&c->d2, &c->d, &c->d, &c->p);
	if (ret) return ret;

	ret = mbedtls_mpi_read_string(&c->B.X, 10, BX_STR);
	if (ret) return ret;
	ret = mbedtls_mpi_read_string(&c->B.Y, 10, BY_STR);
	if (ret) return ret;
	ret = mbedtls_mpi_lset(&c->B.Z, 1);
	if (ret) return ret;
	return fe_mul(&c->B.T, &c->B.X, &c->B.Y, &c->p);
}

static void curve_ctx_free(struct curve_ctx *c)
{
	mbedtls_mpi_free(&c->p);
	mbedtls_mpi_free(&c->d);
	mbedtls_mpi_free(&c->d2);
	mbedtls_mpi_free(&c->L);
	mbedtls_mpi_free(&c->sqrt_m1);
	ge_free(&c->B);
}

/*
 * Decode a compressed point (RFC 8032 §5.1.3): y is the low 255 bits, the
 * top bit of the last byte is x's sign. Recovers x via
 * x^2 = (y^2-1)/(d*y^2+1) mod p, candidate x = x2^((p+3)/8) mod p (p is
 * congruent to 5 mod 8, so this is the standard sqrt formula for this
 * field), corrected by sqrt(-1) if the first candidate doesn't square back
 * to x2. Returns -EINVAL if the 32 bytes don't encode a valid curve point.
 */
static int point_decode(const uint8_t in[32], ge25519 *out, const struct curve_ctx *c)
{
	uint8_t y_bytes[32];
	int x_sign, ret;
	mbedtls_mpi y, y2, u, v, vinv, x2, x, chk, exp, negx, one;

	mbedtls_mpi_init(&y); mbedtls_mpi_init(&y2); mbedtls_mpi_init(&u);
	mbedtls_mpi_init(&v); mbedtls_mpi_init(&vinv); mbedtls_mpi_init(&x2);
	mbedtls_mpi_init(&x); mbedtls_mpi_init(&chk); mbedtls_mpi_init(&exp);
	mbedtls_mpi_init(&negx); mbedtls_mpi_init(&one);

	memcpy(y_bytes, in, 32);
	x_sign = (y_bytes[31] & 0x80u) ? 1 : 0;
	y_bytes[31] &= 0x7Fu;

	ret = mbedtls_mpi_read_binary_le(&y, y_bytes, 32);
	if (ret) goto done;
	if (mbedtls_mpi_cmp_mpi(&y, &c->p) >= 0) { ret = -EINVAL; goto done; }

	ret = mbedtls_mpi_lset(&one, 1);
	if (ret) goto done;
	ret = fe_mul(&y2, &y, &y, &c->p);
	if (ret) goto done;
	ret = fe_sub(&u, &y2, &one, &c->p);
	if (ret) goto done;
	ret = fe_mul(&v, &y2, &c->d, &c->p);
	if (ret) goto done;
	ret = fe_add(&v, &v, &one, &c->p);
	if (ret) goto done;

	ret = fe_inv(&vinv, &v, &c->p);
	if (ret) goto done;
	ret = fe_mul(&x2, &u, &vinv, &c->p);
	if (ret) goto done;

	ret = mbedtls_mpi_add_int(&exp, &c->p, 3);
	if (ret) goto done;
	ret = mbedtls_mpi_shift_r(&exp, 3);
	if (ret) goto done;
	ret = mbedtls_mpi_exp_mod(&x, &x2, &exp, &c->p, NULL);
	if (ret) goto done;

	ret = fe_mul(&chk, &x, &x, &c->p);
	if (ret) goto done;
	if (mbedtls_mpi_cmp_mpi(&chk, &x2) != 0) {
		ret = fe_mul(&x, &x, &c->sqrt_m1, &c->p);
		if (ret) goto done;
		ret = fe_mul(&chk, &x, &x, &c->p);
		if (ret) goto done;
		if (mbedtls_mpi_cmp_mpi(&chk, &x2) != 0) { ret = -EINVAL; goto done; }
	}

	if (mbedtls_mpi_cmp_int(&x, 0) == 0 && x_sign) { ret = -EINVAL; goto done; }

	{
		int xb0 = mbedtls_mpi_get_bit(&x, 0);
		if (xb0 < 0) { ret = xb0; goto done; }
		if (xb0 != x_sign) {
			ret = mbedtls_mpi_sub_mpi(&negx, &c->p, &x);
			if (ret) goto done;
			ret = fe_reduce(&x, &negx, &c->p);
			if (ret) goto done;
		}
	}

	ge_init(out);
	ret = mbedtls_mpi_copy(&out->X, &x);
	if (ret) goto done;
	ret = mbedtls_mpi_copy(&out->Y, &y);
	if (ret) goto done;
	ret = mbedtls_mpi_lset(&out->Z, 1);
	if (ret) goto done;
	ret = fe_mul(&out->T, &x, &y, &c->p);

done:
	mbedtls_mpi_free(&y); mbedtls_mpi_free(&y2); mbedtls_mpi_free(&u);
	mbedtls_mpi_free(&v); mbedtls_mpi_free(&vinv); mbedtls_mpi_free(&x2);
	mbedtls_mpi_free(&x); mbedtls_mpi_free(&chk); mbedtls_mpi_free(&exp);
	mbedtls_mpi_free(&negx); mbedtls_mpi_free(&one);
	return ret;
}

int ed25519_keygen(const uint8_t seed[32], uint8_t pub[32])
{
	struct curve_ctx c;
	uint8_t h[64];
	uint8_t a_bytes[32];
	mbedtls_mpi a;
	ge25519 A;
	int ret;

	memset(&c, 0, sizeof(c));
	mbedtls_mpi_init(&a);
	ge_init(&A);

	ret = curve_ctx_init(&c);
	if (ret) goto done;

	ret = sha512_oneshot(seed, 32, h);
	if (ret) goto done;

	memcpy(a_bytes, h, 32);
	ed25519_clamp(a_bytes);

	ret = mbedtls_mpi_read_binary_le(&a, a_bytes, 32);
	if (ret) goto done;

	ret = scalarmult(&A, &a, &c.B, &c.p, &c.d2);
	if (ret) goto done;

	ret = point_encode(&A, &c.p, pub);

done:
	memset(h, 0, sizeof(h));
	memset(a_bytes, 0, sizeof(a_bytes));
	mbedtls_mpi_free(&a);
	ge_free(&A);
	curve_ctx_free(&c);
	return ret ? -EIO : 0;
}

int ed25519_sign(const uint8_t seed[32], const uint8_t *msg, uint32_t msg_len,
                  uint8_t sig[64])
{
	struct curve_ctx c;
	uint8_t h[64];
	uint8_t a_bytes[32];
	uint8_t prefix[32];
	uint8_t pub[32];
	uint8_t r_hash[64];
	uint8_t k_hash[64];
	uint8_t r_enc[32];
	mbedtls_mpi a, r, k, s, tmp;
	ge25519 A, R;
	uint8_t *buf = NULL;
	int ret;

	memset(&c, 0, sizeof(c));
	mbedtls_mpi_init(&a); mbedtls_mpi_init(&r); mbedtls_mpi_init(&k);
	mbedtls_mpi_init(&s); mbedtls_mpi_init(&tmp);
	ge_init(&A); ge_init(&R);

	buf = k_malloc(64u + (size_t)msg_len);
	if (!buf) { ret = -1; goto done; }

	ret = curve_ctx_init(&c);
	if (ret) goto done;

	ret = sha512_oneshot(seed, 32, h);
	if (ret) goto done;
	memcpy(a_bytes, h, 32);
	memcpy(prefix, h + 32, 32);
	ed25519_clamp(a_bytes);

	ret = mbedtls_mpi_read_binary_le(&a, a_bytes, 32);
	if (ret) goto done;

	ret = scalarmult(&A, &a, &c.B, &c.p, &c.d2);
	if (ret) goto done;
	ret = point_encode(&A, &c.p, pub);
	if (ret) goto done;

	/* r = SHA512(prefix || msg) mod L */
	memcpy(buf, prefix, 32);
	if (msg_len) memcpy(buf + 32, msg, msg_len);
	ret = sha512_oneshot(buf, 32u + msg_len, r_hash);
	if (ret) goto done;
	ret = scalar_from_hash_le(&r, r_hash, &c.L);
	if (ret) goto done;

	ret = scalarmult(&R, &r, &c.B, &c.p, &c.d2);
	if (ret) goto done;
	ret = point_encode(&R, &c.p, r_enc);
	if (ret) goto done;

	/* k = SHA512(R || pub || msg) mod L */
	memcpy(buf, r_enc, 32);
	memcpy(buf + 32, pub, 32);
	if (msg_len) memcpy(buf + 64, msg, msg_len);
	ret = sha512_oneshot(buf, 64u + msg_len, k_hash);
	if (ret) goto done;
	ret = scalar_from_hash_le(&k, k_hash, &c.L);
	if (ret) goto done;

	/* S = (r + k*a) mod L */
	ret = mbedtls_mpi_mul_mpi(&tmp, &k, &a);
	if (ret) goto done;
	ret = mbedtls_mpi_add_mpi(&tmp, &tmp, &r);
	if (ret) goto done;
	ret = mbedtls_mpi_mod_mpi(&s, &tmp, &c.L);
	if (ret) goto done;

	memcpy(sig, r_enc, 32);
	ret = mbedtls_mpi_write_binary_le(&s, sig + 32, 32);

done:
	if (buf) {
		memset(buf, 0, 64u + (size_t)msg_len);
		k_free(buf);
	}
	memset(h, 0, sizeof(h));
	memset(a_bytes, 0, sizeof(a_bytes));
	memset(prefix, 0, sizeof(prefix));
	memset(r_hash, 0, sizeof(r_hash));
	memset(k_hash, 0, sizeof(k_hash));
	mbedtls_mpi_free(&a); mbedtls_mpi_free(&r); mbedtls_mpi_free(&k);
	mbedtls_mpi_free(&s); mbedtls_mpi_free(&tmp);
	ge_free(&A); ge_free(&R);
	curve_ctx_free(&c);
	return ret ? -EIO : 0;
}

int ed25519_verify(const uint8_t pub[32], const uint8_t *msg, uint32_t msg_len,
                    const uint8_t sig[64])
{
	struct curve_ctx c;
	mbedtls_mpi s, k;
	ge25519 A, R, sB, kA, rhs;
	uint8_t *buf = NULL;
	uint8_t k_hash[64];
	uint8_t lhs_enc[32], rhs_enc[32];
	int ret;

	memset(&c, 0, sizeof(c));
	mbedtls_mpi_init(&s); mbedtls_mpi_init(&k);
	ge_init(&A); ge_init(&R); ge_init(&sB); ge_init(&kA); ge_init(&rhs);

	ret = curve_ctx_init(&c);
	if (ret) goto done;

	/* Reject non-canonical S per RFC 8032 §5.1.7 (strict verification) —
	 * without this a signature could be malleable (S, S+L both "work"). */
	ret = mbedtls_mpi_read_binary_le(&s, sig + 32, 32);
	if (ret) goto done;
	if (mbedtls_mpi_cmp_mpi(&s, &c.L) >= 0) { ret = -EINVAL; goto done; }

	ret = point_decode(pub, &A, &c);
	if (ret) goto done;
	ret = point_decode(sig, &R, &c);
	if (ret) goto done;

	buf = k_malloc(64u + (size_t)msg_len);
	if (!buf) { ret = -ENOMEM; goto done; }
	memcpy(buf, sig, 32);        /* R, taken from the signature as-is */
	memcpy(buf + 32, pub, 32);
	if (msg_len) memcpy(buf + 64, msg, msg_len);
	ret = sha512_oneshot(buf, 64u + msg_len, k_hash);
	if (ret) goto done;
	ret = scalar_from_hash_le(&k, k_hash, &c.L);
	if (ret) goto done;

	/* Check [S]B == R + [k]A by comparing encoded points — avoids having
	 * to normalize/compare projective coordinates directly. */
	ret = scalarmult(&sB, &s, &c.B, &c.p, &c.d2);
	if (ret) goto done;
	ret = scalarmult(&kA, &k, &A, &c.p, &c.d2);
	if (ret) goto done;
	ret = point_add(&rhs, &R, &kA, &c.p, &c.d2);
	if (ret) goto done;
	ret = point_encode(&sB, &c.p, lhs_enc);
	if (ret) goto done;
	ret = point_encode(&rhs, &c.p, rhs_enc);
	if (ret) goto done;

	ret = (memcmp(lhs_enc, rhs_enc, 32) == 0) ? 0 : -EINVAL;

done:
	if (buf) {
		memset(buf, 0, 64u + (size_t)msg_len);
		k_free(buf);
	}
	mbedtls_mpi_free(&s); mbedtls_mpi_free(&k);
	ge_free(&A); ge_free(&R); ge_free(&sB); ge_free(&kA); ge_free(&rhs);
	curve_ctx_free(&c);
	return ret;
}

int ed25519_self_test(void)
{
	/* RFC 8032 sec 7.1, test vector 1 */
	static const uint8_t seed[32] = {
		0x9d,0x61,0xb1,0x9d,0xef,0xfd,0x5a,0x60,0xba,0x84,0x4a,0xf4,0x92,0xec,0x2c,0xc4,
		0x44,0x49,0xc5,0x69,0x7b,0x32,0x69,0x19,0x70,0x3b,0xac,0x03,0x1c,0xae,0x7f,0x60
	};
	static const uint8_t expect_pub[32] = {
		0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,
		0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a
	};
	static const uint8_t expect_sig[64] = {
		0xe5,0x56,0x43,0x00,0xc3,0x60,0xac,0x72,0x90,0x86,0xe2,0xcc,0x80,0x6e,0x82,0x8a,
		0x84,0x87,0x7f,0x1e,0xb8,0xe5,0xd9,0x74,0xd8,0x73,0xe0,0x65,0x22,0x49,0x01,0x55,
		0x5f,0xb8,0x82,0x15,0x90,0xa3,0x3b,0xac,0xc6,0x1e,0x39,0x70,0x1c,0xf9,0xb4,0x6b,
		0xd2,0x5b,0xf5,0xf0,0x59,0x5b,0xbe,0x24,0x65,0x51,0x41,0x43,0x8e,0x7a,0x10,0x0b
	};
	uint8_t pub[32], sig[64];

	if (ed25519_keygen(seed, pub) != 0) return -1;
	if (memcmp(pub, expect_pub, 32) != 0) return -2;
	/* message is empty for test vector 1 */
	if (ed25519_sign(seed, NULL, 0, sig) != 0) return -3;
	if (memcmp(sig, expect_sig, 64) != 0) return -4;
	if (ed25519_verify(pub, NULL, 0, sig) != 0) return -9;
	{
		uint8_t bad_sig[64];
		memcpy(bad_sig, sig, 64);
		bad_sig[0] ^= 1;
		if (ed25519_verify(pub, NULL, 0, bad_sig) == 0) return -10;
	}
	{
		static const uint8_t not_empty[1] = { 'x' };
		if (ed25519_verify(pub, not_empty, 1, sig) == 0) return -11;
	}

	/* RFC 8032 sec 7.1, "TEST 1024" — 1023-octet message. This is the only
	 * vector whose SHA512(R||A||M) computation exceeds one 128-byte block
	 * (32+32+1023 bytes), so it's the only thing that actually exercises
	 * sha512_oneshot()'s multi-block path; test vector 1 above never does. */
	static const uint8_t seed2[32] = {
		0xf5,0xe5,0x76,0x7c,0xf1,0x53,0x31,0x95,0x17,0x63,0x0f,0x22,0x68,0x76,0xb8,0x6c,
		0x81,0x60,0xcc,0x58,0x3b,0xc0,0x13,0x74,0x4c,0x6b,0xf2,0x55,0xf5,0xcc,0x0e,0xe5,
	};
	static const uint8_t expect_pub2[32] = {
		0x27,0x81,0x17,0xfc,0x14,0x4c,0x72,0x34,0x0f,0x67,0xd0,0xf2,0x31,0x6e,0x83,0x86,
		0xce,0xff,0xbf,0x2b,0x24,0x28,0xc9,0xc5,0x1f,0xef,0x7c,0x59,0x7f,0x1d,0x42,0x6e,
	};
	static const uint8_t expect_sig2[64] = {
		0x3f,0xa6,0xb2,0x68,0x5a,0xb6,0x6b,0x3b,0x31,0xcc,0x07,0x82,0x11,0x39,0xfa,0x23,
		0x97,0xc0,0x44,0x8d,0x65,0xad,0xbb,0xa0,0x63,0xb1,0xb7,0x1d,0x5c,0x7a,0x02,0xe2,
		0x99,0xe7,0x64,0x2a,0xb5,0x10,0x9c,0x3e,0x16,0xb9,0xfc,0x5b,0xa4,0x83,0xfb,0x5d,
		0xe2,0x1e,0xf0,0x90,0xc4,0x9a,0xae,0xc8,0x18,0x6e,0x9b,0xf8,0xf3,0xf7,0xbd,0x06,
	};
	static const uint8_t msg2[1023] = {
		0x08,0xb8,0xb2,0xb7,0x33,0x42,0x42,0x43,0x76,0x0f,0xe4,0x26,0xa4,0xb5,0x49,0x08,
		0x63,0x21,0x1a,0x66,0xc2,0xf6,0x59,0x1e,0xab,0xd3,0x34,0x5e,0x3e,0x4e,0xb9,0x8f,
		0xa6,0xe2,0x64,0xbf,0x09,0xef,0xe1,0x2e,0xe5,0x0f,0x8f,0x54,0xe9,0xf7,0x7b,0x1e,
		0x35,0x5f,0x6c,0x50,0x54,0x4e,0x23,0xfb,0x14,0x33,0xdd,0xf7,0x3b,0xe8,0x4d,0x87,
		0x9d,0xe7,0xc0,0x04,0x6d,0xc4,0x99,0x6d,0x9e,0x77,0x3f,0x4b,0xc9,0xef,0xe5,0x73,
		0x88,0x29,0xad,0xb2,0x6c,0x81,0xb3,0x7c,0x93,0xa1,0xb2,0x70,0xb2,0x03,0x29,0xd6,
		0x58,0x67,0x5f,0xc6,0xea,0x53,0x4e,0x08,0x10,0xa4,0x43,0x28,0x26,0xbf,0x58,0xc9,
		0x41,0xef,0xb6,0x5d,0x57,0xa3,0x38,0xbb,0xd2,0xe2,0x66,0x40,0xf8,0x9f,0xfb,0xc1,
		0xa8,0x58,0xef,0xcb,0x85,0x50,0xee,0x3a,0x5e,0x19,0x98,0xbd,0x17,0x7e,0x93,0xa7,
		0x36,0x3c,0x34,0x4f,0xe6,0xb1,0x99,0xee,0x5d,0x02,0xe8,0x2d,0x52,0x2c,0x4f,0xeb,
		0xa1,0x54,0x52,0xf8,0x02,0x88,0xa8,0x21,0xa5,0x79,0x11,0x6e,0xc6,0xda,0xd2,0xb3,
		0xb3,0x10,0xda,0x90,0x34,0x01,0xaa,0x62,0x10,0x0a,0xb5,0xd1,0xa3,0x65,0x53,0xe0,
		0x62,0x03,0xb3,0x38,0x90,0xcc,0x9b,0x83,0x2f,0x79,0xef,0x80,0x56,0x0c,0xcb,0x9a,
		0x39,0xce,0x76,0x79,0x67,0xed,0x62,0x8c,0x6a,0xd5,0x73,0xcb,0x11,0x6d,0xbe,0xff,
		0xef,0xd7,0x54,0x99,0xda,0x96,0xbd,0x68,0xa8,0xa9,0x7b,0x92,0x8a,0x8b,0xbc,0x10,
		0x3b,0x66,0x21,0xfc,0xde,0x2b,0xec,0xa1,0x23,0x1d,0x20,0x6b,0xe6,0xcd,0x9e,0xc7,
		0xaf,0xf6,0xf6,0xc9,0x4f,0xcd,0x72,0x04,0xed,0x34,0x55,0xc6,0x8c,0x83,0xf4,0xa4,
		0x1d,0xa4,0xaf,0x2b,0x74,0xef,0x5c,0x53,0xf1,0xd8,0xac,0x70,0xbd,0xcb,0x7e,0xd1,
		0x85,0xce,0x81,0xbd,0x84,0x35,0x9d,0x44,0x25,0x4d,0x95,0x62,0x9e,0x98,0x55,0xa9,
		0x4a,0x7c,0x19,0x58,0xd1,0xf8,0xad,0xa5,0xd0,0x53,0x2e,0xd8,0xa5,0xaa,0x3f,0xb2,
		0xd1,0x7b,0xa7,0x0e,0xb6,0x24,0x8e,0x59,0x4e,0x1a,0x22,0x97,0xac,0xbb,0xb3,0x9d,
		0x50,0x2f,0x1a,0x8c,0x6e,0xb6,0xf1,0xce,0x22,0xb3,0xde,0x1a,0x1f,0x40,0xcc,0x24,
		0x55,0x41,0x19,0xa8,0x31,0xa9,0xaa,0xd6,0x07,0x9c,0xad,0x88,0x42,0x5d,0xe6,0xbd,
		0xe1,0xa9,0x18,0x7e,0xbb,0x60,0x92,0xcf,0x67,0xbf,0x2b,0x13,0xfd,0x65,0xf2,0x70,
		0x88,0xd7,0x8b,0x7e,0x88,0x3c,0x87,0x59,0xd2,0xc4,0xf5,0xc6,0x5a,0xdb,0x75,0x53,
		0x87,0x8a,0xd5,0x75,0xf9,0xfa,0xd8,0x78,0xe8,0x0a,0x0c,0x9b,0xa6,0x3b,0xcb,0xcc,
		0x27,0x32,0xe6,0x94,0x85,0xbb,0xc9,0xc9,0x0b,0xfb,0xd6,0x24,0x81,0xd9,0x08,0x9b,
		0xec,0xcf,0x80,0xcf,0xe2,0xdf,0x16,0xa2,0xcf,0x65,0xbd,0x92,0xdd,0x59,0x7b,0x07,
		0x07,0xe0,0x91,0x7a,0xf4,0x8b,0xbb,0x75,0xfe,0xd4,0x13,0xd2,0x38,0xf5,0x55,0x5a,
		0x7a,0x56,0x9d,0x80,0xc3,0x41,0x4a,0x8d,0x08,0x59,0xdc,0x65,0xa4,0x61,0x28,0xba,
		0xb2,0x7a,0xf8,0x7a,0x71,0x31,0x4f,0x31,0x8c,0x78,0x2b,0x23,0xeb,0xfe,0x80,0x8b,
		0x82,0xb0,0xce,0x26,0x40,0x1d,0x2e,0x22,0xf0,0x4d,0x83,0xd1,0x25,0x5d,0xc5,0x1a,
		0xdd,0xd3,0xb7,0x5a,0x2b,0x1a,0xe0,0x78,0x45,0x04,0xdf,0x54,0x3a,0xf8,0x96,0x9b,
		0xe3,0xea,0x70,0x82,0xff,0x7f,0xc9,0x88,0x8c,0x14,0x4d,0xa2,0xaf,0x58,0x42,0x9e,
		0xc9,0x60,0x31,0xdb,0xca,0xd3,0xda,0xd9,0xaf,0x0d,0xcb,0xaa,0xaf,0x26,0x8c,0xb8,
		0xfc,0xff,0xea,0xd9,0x4f,0x3c,0x7c,0xa4,0x95,0xe0,0x56,0xa9,0xb4,0x7a,0xcd,0xb7,
		0x51,0xfb,0x73,0xe6,0x66,0xc6,0xc6,0x55,0xad,0xe8,0x29,0x72,0x97,0xd0,0x7a,0xd1,
		0xba,0x5e,0x43,0xf1,0xbc,0xa3,0x23,0x01,0x65,0x13,0x39,0xe2,0x29,0x04,0xcc,0x8c,
		0x42,0xf5,0x8c,0x30,0xc0,0x4a,0xaf,0xdb,0x03,0x8d,0xda,0x08,0x47,0xdd,0x98,0x8d,
		0xcd,0xa6,0xf3,0xbf,0xd1,0x5c,0x4b,0x4c,0x45,0x25,0x00,0x4a,0xa0,0x6e,0xef,0xf8,
		0xca,0x61,0x78,0x3a,0xac,0xec,0x57,0xfb,0x3d,0x1f,0x92,0xb0,0xfe,0x2f,0xd1,0xa8,
		0x5f,0x67,0x24,0x51,0x7b,0x65,0xe6,0x14,0xad,0x68,0x08,0xd6,0xf6,0xee,0x34,0xdf,
		0xf7,0x31,0x0f,0xdc,0x82,0xae,0xbf,0xd9,0x04,0xb0,0x1e,0x1d,0xc5,0x4b,0x29,0x27,
		0x09,0x4b,0x2d,0xb6,0x8d,0x6f,0x90,0x3b,0x68,0x40,0x1a,0xde,0xbf,0x5a,0x7e,0x08,
		0xd7,0x8f,0xf4,0xef,0x5d,0x63,0x65,0x3a,0x65,0x04,0x0c,0xf9,0xbf,0xd4,0xac,0xa7,
		0x98,0x4a,0x74,0xd3,0x71,0x45,0x98,0x67,0x80,0xfc,0x0b,0x16,0xac,0x45,0x16,0x49,
		0xde,0x61,0x88,0xa7,0xdb,0xdf,0x19,0x1f,0x64,0xb5,0xfc,0x5e,0x2a,0xb4,0x7b,0x57,
		0xf7,0xf7,0x27,0x6c,0xd4,0x19,0xc1,0x7a,0x3c,0xa8,0xe1,0xb9,0x39,0xae,0x49,0xe4,
		0x88,0xac,0xba,0x6b,0x96,0x56,0x10,0xb5,0x48,0x01,0x09,0xc8,0xb1,0x7b,0x80,0xe1,
		0xb7,0xb7,0x50,0xdf,0xc7,0x59,0x8d,0x5d,0x50,0x11,0xfd,0x2d,0xcc,0x56,0x00,0xa3,
		0x2e,0xf5,0xb5,0x2a,0x1e,0xcc,0x82,0x0e,0x30,0x8a,0xa3,0x42,0x72,0x1a,0xac,0x09,
		0x43,0xbf,0x66,0x86,0xb6,0x4b,0x25,0x79,0x37,0x65,0x04,0xcc,0xc4,0x93,0xd9,0x7e,
		0x6a,0xed,0x3f,0xb0,0xf9,0xcd,0x71,0xa4,0x3d,0xd4,0x97,0xf0,0x1f,0x17,0xc0,0xe2,
		0xcb,0x37,0x97,0xaa,0x2a,0x2f,0x25,0x66,0x56,0x16,0x8e,0x6c,0x49,0x6a,0xfc,0x5f,
		0xb9,0x32,0x46,0xf6,0xb1,0x11,0x63,0x98,0xa3,0x46,0xf1,0xa6,0x41,0xf3,0xb0,0x41,
		0xe9,0x89,0xf7,0x91,0x4f,0x90,0xcc,0x2c,0x7f,0xff,0x35,0x78,0x76,0xe5,0x06,0xb5,
		0x0d,0x33,0x4b,0xa7,0x7c,0x22,0x5b,0xc3,0x07,0xba,0x53,0x71,0x52,0xf3,0xf1,0x61,
		0x0e,0x4e,0xaf,0xe5,0x95,0xf6,0xd9,0xd9,0x0d,0x11,0xfa,0xa9,0x33,0xa1,0x5e,0xf1,
		0x36,0x95,0x46,0x86,0x8a,0x7f,0x3a,0x45,0xa9,0x67,0x68,0xd4,0x0f,0xd9,0xd0,0x34,
		0x12,0xc0,0x91,0xc6,0x31,0x5c,0xf4,0xfd,0xe7,0xcb,0x68,0x60,0x69,0x37,0x38,0x0d,
		0xb2,0xea,0xaa,0x70,0x7b,0x4c,0x41,0x85,0xc3,0x2e,0xdd,0xcd,0xd3,0x06,0x70,0x5e,
		0x4d,0xc1,0xff,0xc8,0x72,0xee,0xee,0x47,0x5a,0x64,0xdf,0xac,0x86,0xab,0xa4,0x1c,
		0x06,0x18,0x98,0x3f,0x87,0x41,0xc5,0xef,0x68,0xd3,0xa1,0x01,0xe8,0xa3,0xb8,0xca,
		0xc6,0x0c,0x90,0x5c,0x15,0xfc,0x91,0x08,0x40,0xb9,0x4c,0x00,0xa0,0xb9,0xd0,
	};
	uint8_t pub2[32], sig2[64];
	if (ed25519_keygen(seed2, pub2) != 0) return -5;
	if (memcmp(pub2, expect_pub2, 32) != 0) return -6;
	if (ed25519_sign(seed2, msg2, sizeof(msg2), sig2) != 0) return -7;
	if (memcmp(sig2, expect_sig2, 64) != 0) return -8;

	return 0;
}
