/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mesh_crypto.h"
#include "ed25519.h"
#include <string.h>
#include <errno.h>
#include <zephyr/sys/util.h>
#include <zephyr/random/random.h>

#if defined(CONFIG_AKIRA_MESH_E2E_CRYPTO)
#include <psa/crypto.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include "settings/settings.h"

#define AES_KEY_SIZE_256 32
#define AES_BLOCK_SIZE   16
#define SHA256_DIGEST_SIZE 32
#define HMAC_BLOCK 64

#define MESH_ID_PRIV_KEY "mesh_id_priv"
#define MESH_ID_PUB_KEY  "mesh_id_pub"
#define MESH_SIGN_PRIV_KEY "mesh_sign_priv"
#define MESH_SIGN_PUB_KEY  "mesh_sign_pub"

static void bytes_to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i]     = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0F];
    }
    out[2 * len] = '\0';
}

static bool hex_to_bytes(const char *in, size_t out_len, uint8_t *out)
{
    if (strlen(in) != 2 * out_len) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        out[i] = (uint8_t)HEX_TO_BYTE(in[2 * i], in[2 * i + 1]);
    }
    return true;
}

int mesh_crypto_p256_keygen(uint8_t priv_out[MESH_CRYPTO_PRIV_LEN],
                            uint8_t pub_out[MESH_CRYPTO_PUB_LEN])
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return -EIO;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);

    mbedtls_svc_key_id_t key_id;
    psa_status_t st = psa_generate_key(&attr, &key_id);
    if (st != PSA_SUCCESS) {
        return -EIO;
    }

    size_t priv_len = 0, pub_len = 0;
    st = psa_export_key(key_id, priv_out, MESH_CRYPTO_PRIV_LEN, &priv_len);
    if (st == PSA_SUCCESS) {
        st = psa_export_public_key(key_id, pub_out, MESH_CRYPTO_PUB_LEN, &pub_len);
    }
    psa_destroy_key(key_id);

    if (st != PSA_SUCCESS || priv_len != MESH_CRYPTO_PRIV_LEN ||
        pub_len != MESH_CRYPTO_PUB_LEN) {
        memset(priv_out, 0, MESH_CRYPTO_PRIV_LEN);
        return -EIO;
    }
    return 0;
}

int mesh_crypto_p256_ecdh(const uint8_t priv[MESH_CRYPTO_PRIV_LEN],
                          const uint8_t peer_pub[MESH_CRYPTO_PUB_LEN],
                          uint8_t shared_out[MESH_CRYPTO_SHARED_LEN])
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return -EIO;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);

    mbedtls_svc_key_id_t key_id;
    psa_status_t st = psa_import_key(&attr, priv, MESH_CRYPTO_PRIV_LEN, &key_id);
    if (st != PSA_SUCCESS) {
        return -EIO;
    }

    size_t out_len = 0;
    st = psa_raw_key_agreement(PSA_ALG_ECDH, key_id, peer_pub, MESH_CRYPTO_PUB_LEN,
                               shared_out, MESH_CRYPTO_SHARED_LEN, &out_len);
    psa_destroy_key(key_id);

    if (st != PSA_SUCCESS || out_len != MESH_CRYPTO_SHARED_LEN) {
        memset(shared_out, 0, MESH_CRYPTO_SHARED_LEN);
        return -EIO;
    }
    return 0;
}

void mesh_crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                             const uint8_t *data, size_t data_len,
                             uint8_t out[32])
{
    uint8_t k_ipad[HMAC_BLOCK], k_opad[HMAC_BLOCK];
    memset(k_ipad, 0x36, HMAC_BLOCK);
    memset(k_opad, 0x5c, HMAC_BLOCK);
    for (size_t i = 0; i < key_len && i < HMAC_BLOCK; i++) {
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }

    uint8_t inner[SHA256_DIGEST_SIZE];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, k_ipad, HMAC_BLOCK);
    mbedtls_sha256_update(&ctx, data, data_len);
    mbedtls_sha256_finish(&ctx, inner);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, k_opad, HMAC_BLOCK);
    mbedtls_sha256_update(&ctx, inner, SHA256_DIGEST_SIZE);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);

    memset(k_ipad, 0, HMAC_BLOCK);
    memset(k_opad, 0, HMAC_BLOCK);
    memset(inner, 0, sizeof(inner));
}

int mesh_crypto_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                            const uint8_t *salt, size_t salt_len,
                            const uint8_t *info, size_t info_len,
                            uint8_t *okm_out, size_t okm_len)
{
    if (okm_len > 64 || info_len > 64) {
        return -EINVAL;
    }

    uint8_t prk[32];
    mesh_crypto_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);

    uint8_t t_prev[32];
    size_t t_prev_len = 0;
    uint8_t block_in[sizeof(t_prev) + 64 + 1];
    size_t produced = 0;
    uint8_t counter = 1;

    while (produced < okm_len) {
        size_t off = 0;
        memcpy(block_in + off, t_prev, t_prev_len);
        off += t_prev_len;
        memcpy(block_in + off, info, info_len);
        off += info_len;
        block_in[off++] = counter;

        uint8_t t[32];
        mesh_crypto_hmac_sha256(prk, sizeof(prk), block_in, off, t);

        size_t take = MIN(sizeof(t), okm_len - produced);
        memcpy(okm_out + produced, t, take);
        produced += take;

        memcpy(t_prev, t, sizeof(t));
        t_prev_len = sizeof(t);
        counter++;
        memset(t, 0, sizeof(t));
    }

    memset(prk, 0, sizeof(prk));
    memset(t_prev, 0, sizeof(t_prev));
    memset(block_in, 0, sizeof(block_in));
    return 0;
}

int mesh_crypto_aes256_ctr(const uint8_t key[32], const uint8_t nonce[16],
                           const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t nonce_ctr[AES_BLOCK_SIZE];
    uint8_t stream_block[AES_BLOCK_SIZE];
    size_t nc_off = 0;
    memcpy(nonce_ctr, nonce, AES_BLOCK_SIZE);
    memset(stream_block, 0, sizeof(stream_block));

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int ret = mbedtls_aes_setkey_enc(&ctx, key, 256);
    if (ret == 0) {
        ret = mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce_ctr, stream_block, in, out);
    }
    mbedtls_aes_free(&ctx);
    memset(nonce_ctr, 0, sizeof(nonce_ctr));
    memset(stream_block, 0, sizeof(stream_block));
    return (ret == 0) ? 0 : -EIO;
}

bool mesh_crypto_const_time_eq(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

int mesh_crypto_identity_init(uint8_t priv_out[MESH_CRYPTO_PRIV_LEN],
                              uint8_t pub_out[MESH_CRYPTO_PUB_LEN])
{
    char priv_hex[2 * MESH_CRYPTO_PRIV_LEN + 1];
    char pub_hex[2 * MESH_CRYPTO_PUB_LEN + 1];

    int rp = akira_settings_get(MESH_ID_PRIV_KEY, priv_hex, sizeof(priv_hex));
    int ru = akira_settings_get(MESH_ID_PUB_KEY, pub_hex, sizeof(pub_hex));
    if (rp == 0 && ru == 0 &&
        hex_to_bytes(priv_hex, MESH_CRYPTO_PRIV_LEN, priv_out) &&
        hex_to_bytes(pub_hex, MESH_CRYPTO_PUB_LEN, pub_out)) {
        memset(priv_hex, 0, sizeof(priv_hex));
        return 0;
    }

    int ret = mesh_crypto_p256_keygen(priv_out, pub_out);
    if (ret != 0) {
        memset(priv_hex, 0, sizeof(priv_hex));
        return ret;
    }

    bytes_to_hex(priv_out, MESH_CRYPTO_PRIV_LEN, priv_hex);
    bytes_to_hex(pub_out, MESH_CRYPTO_PUB_LEN, pub_hex);
    int sp = akira_settings_set(MESH_ID_PRIV_KEY, priv_hex, 1 /* encrypted */);
    int su = akira_settings_set(MESH_ID_PUB_KEY, pub_hex, 0 /* not secret */);
    memset(priv_hex, 0, sizeof(priv_hex));
    if (sp != 0 || su != 0) {
        return -EIO;
    }
    return 0;
}

int mesh_crypto_signing_identity_init(uint8_t priv_out[MESH_CRYPTO_SIGN_PRIV_LEN],
                                      uint8_t pub_out[MESH_CRYPTO_SIGN_PUB_LEN])
{
    char priv_hex[2 * MESH_CRYPTO_SIGN_PRIV_LEN + 1];
    char pub_hex[2 * MESH_CRYPTO_SIGN_PUB_LEN + 1];

    int rp = akira_settings_get(MESH_SIGN_PRIV_KEY, priv_hex, sizeof(priv_hex));
    int ru = akira_settings_get(MESH_SIGN_PUB_KEY, pub_hex, sizeof(pub_hex));
    if (rp == 0 && ru == 0 &&
        hex_to_bytes(priv_hex, MESH_CRYPTO_SIGN_PRIV_LEN, priv_out) &&
        hex_to_bytes(pub_hex, MESH_CRYPTO_SIGN_PUB_LEN, pub_out)) {
        memset(priv_hex, 0, sizeof(priv_hex));
        return 0;
    }

    /* No PSA support for the twisted-Edwards curve on this target (see
     * ed25519.h) — seed from the same CSPRNG used for the AES-CTR nonce
     * elsewhere in this file, not PSA. */
    sys_csrand_get(priv_out, MESH_CRYPTO_SIGN_PRIV_LEN);
    if (ed25519_keygen(priv_out, pub_out) != 0) {
        memset(priv_out, 0, MESH_CRYPTO_SIGN_PRIV_LEN);
        memset(priv_hex, 0, sizeof(priv_hex));
        return -EIO;
    }

    bytes_to_hex(priv_out, MESH_CRYPTO_SIGN_PRIV_LEN, priv_hex);
    bytes_to_hex(pub_out, MESH_CRYPTO_SIGN_PUB_LEN, pub_hex);
    int sp = akira_settings_set(MESH_SIGN_PRIV_KEY, priv_hex, 1 /* encrypted */);
    int su = akira_settings_set(MESH_SIGN_PUB_KEY, pub_hex, 0 /* not secret */);
    memset(priv_hex, 0, sizeof(priv_hex));
    if (sp != 0 || su != 0) {
        return -EIO;
    }
    return 0;
}

#else /* !CONFIG_AKIRA_MESH_E2E_CRYPTO */

int mesh_crypto_p256_keygen(uint8_t priv_out[MESH_CRYPTO_PRIV_LEN],
                            uint8_t pub_out[MESH_CRYPTO_PUB_LEN])
{
    (void)priv_out; (void)pub_out;
    return -ENOTSUP;
}

int mesh_crypto_p256_ecdh(const uint8_t priv[MESH_CRYPTO_PRIV_LEN],
                          const uint8_t peer_pub[MESH_CRYPTO_PUB_LEN],
                          uint8_t shared_out[MESH_CRYPTO_SHARED_LEN])
{
    (void)priv; (void)peer_pub; (void)shared_out;
    return -ENOTSUP;
}

void mesh_crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                             const uint8_t *data, size_t data_len,
                             uint8_t out[32])
{
    (void)key; (void)key_len; (void)data; (void)data_len;
    memset(out, 0, 32);
}

int mesh_crypto_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                            const uint8_t *salt, size_t salt_len,
                            const uint8_t *info, size_t info_len,
                            uint8_t *okm_out, size_t okm_len)
{
    (void)ikm; (void)ikm_len; (void)salt; (void)salt_len; (void)info; (void)info_len;
    (void)okm_out; (void)okm_len;
    return -ENOTSUP;
}

int mesh_crypto_aes256_ctr(const uint8_t key[32], const uint8_t nonce[16],
                           const uint8_t *in, size_t len, uint8_t *out)
{
    (void)key; (void)nonce; (void)in; (void)len; (void)out;
    return -ENOTSUP;
}

bool mesh_crypto_const_time_eq(const uint8_t *a, const uint8_t *b, size_t len)
{
    (void)a; (void)b; (void)len;
    return false;
}

int mesh_crypto_identity_init(uint8_t priv_out[MESH_CRYPTO_PRIV_LEN],
                              uint8_t pub_out[MESH_CRYPTO_PUB_LEN])
{
    (void)priv_out; (void)pub_out;
    return -ENOTSUP;
}

int mesh_crypto_signing_identity_init(uint8_t priv_out[MESH_CRYPTO_SIGN_PRIV_LEN],
                                      uint8_t pub_out[MESH_CRYPTO_SIGN_PUB_LEN])
{
    (void)priv_out; (void)pub_out;
    return -ENOTSUP;
}

#endif /* CONFIG_AKIRA_MESH_E2E_CRYPTO */
