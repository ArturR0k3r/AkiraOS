/**
 * @file app_signing.c
 * @brief AkiraOS App Signing Implementation
 */

#include "app_signing.h"
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(akira_app_signing, LOG_LEVEL_INF);

// TODO: Integrate with mbedTLS or TinyCrypt
// TODO: Store root CAs in secure NVS partition
// TODO: Add OCSP/CRL checking
// TODO: Add hardware security module support (ESP32 secure boot)

#define MAX_TRUSTED_ROOTS 4

static struct
{
    bool initialized;
    uint8_t root_hashes[MAX_TRUSTED_ROOTS][32];
    int root_count;
} g_signing_state = {0};

int app_signing_init(void)
{
    // TODO: Load trusted roots from NVS
    // TODO: Initialize crypto library

    memset(&g_signing_state, 0, sizeof(g_signing_state));
    g_signing_state.initialized = true;

    LOG_INF("App signing subsystem initialized");
    return 0;
}

int app_verify_signature(const void *binary, size_t size,
                         const akira_app_signature_t *signature)
{
    // TODO: Implement actual signature verification

    if (!binary || size == 0 || !signature)
    {
        return -1;
    }

    if (!g_signing_state.initialized)
    {
        LOG_ERR("Signing not initialized");
        return -2;
    }

    LOG_INF("Verifying signature for %zu byte binary", size);

    switch (signature->algorithm)
    {
    case SIGN_ALG_RSA2048_SHA256:
        // TODO: RSA-2048 verification
        // 1. Compute SHA-256 of binary
        // 2. Decrypt signature with public key
        // 3. Compare hashes
        LOG_WRN("RSA-2048 verification not implemented");
        return -3;

    case SIGN_ALG_ED25519:
        // TODO: Ed25519 verification
        // 1. Use Ed25519 verify function
        LOG_WRN("Ed25519 verification not implemented");
        return -3;

    case SIGN_ALG_NONE:
        LOG_WRN("Unsigned app - allowing for development only");
        // TODO: Add Kconfig option to reject unsigned apps
        return 0;

    default:
        LOG_ERR("Unknown signature algorithm: %d", signature->algorithm);
        return -4;
    }
}

int app_verify_cert_chain(const akira_cert_t *certs, int count)
{
    // TODO: Implement certificate chain verification

    if (!certs || count <= 0)
    {
        return -1;
    }

    LOG_INF("Verifying certificate chain with %d certs", count);

    // TODO: For each certificate in chain:
    // 1. Check validity period
    // 2. Check that issuer matches subject of next cert
    // 3. Verify signature using issuer's public key
    // 4. Check root is trusted

    return -3; // Not implemented
}

bool app_is_root_trusted(const uint8_t *cert_hash)
{
    if (!cert_hash)
    {
        return false;
    }

    for (int i = 0; i < g_signing_state.root_count; i++)
    {
        if (memcmp(g_signing_state.root_hashes[i], cert_hash, 32) == 0)
        {
            return true;
        }
    }

    return false;
}

int app_add_trusted_root(const akira_cert_t *cert)
{
    // TODO: Compute hash of certificate
    // TODO: Store in NVS

    if (!cert || cert->cert_len == 0)
    {
        return -1;
    }

    if (g_signing_state.root_count >= MAX_TRUSTED_ROOTS)
    {
        LOG_ERR("Max trusted roots reached");
        return -2;
    }

    // TODO: Compute SHA-256 of cert_data
    // memcpy(g_signing_state.root_hashes[g_signing_state.root_count], hash, 32);
    g_signing_state.root_count++;

    LOG_INF("Added trusted root (%d total)", g_signing_state.root_count);
    return 0;
}

int app_compute_hash(const void *data, size_t len, uint8_t *hash)
{
    // TODO: Use mbedTLS SHA-256

    if (!data || len == 0 || !hash)
    {
        return -1;
    }

    // TODO: Implement using mbedtls_sha256 or tinycrypt
    /*
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    */

    // Placeholder: zero hash
    memset(hash, 0, 32);

    return -3; // Not implemented
}
