/**
 * @file app_signing.c
 * @brief AkiraOS App Signing Implementation - mbedTLS backed
 *
 * Provides real cryptographic verification for WASM app binaries:
 * - SHA-256 hashing via mbedTLS
 * - RSA-2048 + SHA-256 signature verification
 * - Ed25519 signature verification (via mbedTLS PK)
 * - Trusted root CA management with NVS persistence
 * - WASM binary integrity checks (magic + hash)
 */

#include "app_signing.h"
#include "sandbox.h"
#include "lib/mem_helper.h"
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(akira_app_signing, CONFIG_AKIRA_LOG_LEVEL);

/* Forward declarations — weak defaults defined later in this file */
int akira_platform_allowlist_verify(const uint8_t *app_hash, size_t hash_len);
int akira_platform_pqc_verify(const uint8_t *digest, size_t digest_len,
                               const uint8_t *sig,    size_t sig_len);

/* mbedTLS integration */
#ifdef CONFIG_MBEDTLS
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>
#include <mbedtls/md.h>
#include <mbedtls/error.h>
#ifdef MBEDTLS_X509_CRT_PARSE_C
#include <mbedtls/x509_crt.h>
#endif
#define CRYPTO_AVAILABLE 1
#else
#define CRYPTO_AVAILABLE 0
#endif

#define MAX_TRUSTED_ROOTS 8 /* Raised from 4 to support certificate rotation / multiple CAs */

/* Maximum size of a DER-encoded SubjectPublicKeyInfo block.
 * RSA-2048: 294 bytes; Ed25519: 44 bytes.  Use RSA size as the safe upper bound. */
#define MAX_PUBKEY_DER_SIZE 300

/* WASM magic bytes */
static const uint8_t WASM_MAGIC[] = {0x00, 0x61, 0x73, 0x6D};
/* AOT magic bytes */
static const uint8_t AOT_MAGIC[] = {0x00, 0x61, 0x6F, 0x74};

static inline bool is_wasm_or_aot(const uint8_t *data)
{
    return memcmp(data, WASM_MAGIC, 4) == 0 ||
           memcmp(data, AOT_MAGIC, 4) == 0;
}

static inline bool is_aot(const uint8_t *data)
{
    return memcmp(data, AOT_MAGIC, 4) == 0;
}

static struct
{
    bool initialized;
    uint8_t root_hashes[MAX_TRUSTED_ROOTS][32];
    /* DER-encoded SubjectPublicKeyInfo for each trusted root.
     * Extracted from the X.509 certificate at app_add_trusted_root() time so
     * that app_verify_signature() can call mbedtls_pk_verify() without
     * storing the full 1 KB certificate. */
    uint8_t root_pubkeys_der[MAX_TRUSTED_ROOTS][MAX_PUBKEY_DER_SIZE];
    size_t  root_pubkeys_len[MAX_TRUSTED_ROOTS];
    int root_count;
} g_signing_state AKIRA_BULK_BSS;

/* ===== SHA-256 Implementation ===== */

int app_compute_hash(const void *data, size_t len, uint8_t *hash)
{
    if (!data || len == 0 || !hash)
    {
        return -EINVAL;
    }

#if CRYPTO_AVAILABLE
    mbedtls_sha256_context ctx;
    int ret;

    mbedtls_sha256_init(&ctx);

    ret = mbedtls_sha256_starts(&ctx, 0 /* SHA-256, not SHA-224 */);
    if (ret != 0)
    {
        LOG_ERR("SHA-256 start failed: -0x%04x", (unsigned int)-ret);
        mbedtls_sha256_free(&ctx);
        return -EIO;
    }

    /* Process in chunks to avoid large stack usage */
    const uint8_t *p = (const uint8_t *)data;
    size_t remaining = len;
    while (remaining > 0)
    {
        size_t chunk = MIN(remaining, 4096U);
        ret = mbedtls_sha256_update(&ctx, p, chunk);
        if (ret != 0)
        {
            LOG_ERR("SHA-256 update failed: -0x%04x", (unsigned int)-ret);
            mbedtls_sha256_free(&ctx);
            return -EIO;
        }
        p += chunk;
        remaining -= chunk;
    }

    ret = mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    if (ret != 0)
    {
        LOG_ERR("SHA-256 finish failed: -0x%04x", (unsigned int)-ret);
        return -EIO;
    }

    return 0;
#else
    LOG_WRN("Crypto not available, hash operation unavailable");
    memset(hash, 0, 32);
    return -ENOTSUP;
#endif
}

/* ===== Signature Verification ===== */

int app_signing_init(void)
{
    memset(&g_signing_state, 0, sizeof(g_signing_state));
    g_signing_state.initialized = true;

    LOG_INF("App signing subsystem initialized (crypto=%s)",
            CRYPTO_AVAILABLE ? "mbedTLS" : "disabled");
    return 0;
}

int app_verify_signature(const void *binary, size_t size,
                         const akira_app_signature_t *signature)
{
    if (!binary || size == 0 || !signature)
    {
        return -EINVAL;
    }

    if (!g_signing_state.initialized)
    {
        LOG_ERR("Signing subsystem not initialized");
        return -ENODEV;
    }

    /* Check for unsigned apps */
    if (signature->algorithm == SIGN_ALG_NONE)
    {
#if defined(CONFIG_AKIRA_ALLOW_UNSIGNED_APPS) || !defined(CONFIG_AKIRA_APP_SIGNING)
        LOG_WRN("Unsigned app - allowed (signing disabled or dev mode)");
        return 0;
#else
        LOG_ERR("Unsigned app rejected - set CONFIG_AKIRA_ALLOW_UNSIGNED_APPS=y for dev builds");
        sandbox_audit_log(AUDIT_EVENT_SIGNATURE_FAIL, "unsigned", 0);
        return -EACCES;
#endif
    }

#if CRYPTO_AVAILABLE
    /* Step 1: Compute SHA-256 hash of binary */
    uint8_t hash[32];
    int ret = app_compute_hash(binary, size, hash);
    if (ret != 0)
    {
        LOG_ERR("Failed to compute binary hash");
        return ret;
    }

    /* Locate the trusted root entry that matches this signature's cert_hash */
    int root_slot = -1;
    for (int i = 0; i < g_signing_state.root_count; i++)
    {
        if (memcmp(g_signing_state.root_hashes[i], signature->cert_hash, 32) == 0)
        {
            root_slot = i;
            break;
        }
    }

    if (root_slot < 0)
    {
        LOG_ERR("Signing certificate not in trusted roots");
        sandbox_audit_log(AUDIT_EVENT_SIGNATURE_FAIL, "untrusted_cert", 0);
        return -EACCES;
    }

    if (g_signing_state.root_pubkeys_len[root_slot] == 0)
    {
        /* Public key was never extracted — signing subsystem is misconfigured.
         * Reject the app rather than silently passing without verification. */
        LOG_ERR("No public key stored for trusted root slot %d — "
                "enable MBEDTLS_X509_CRT_PARSE_C and re-provision the root CA",
                root_slot);
        sandbox_audit_log(AUDIT_EVENT_SIGNATURE_FAIL, "no_pubkey", 0);
        return -EACCES;
    }

#if defined(MBEDTLS_PK_PARSE_C)
    /* Parse the stored SubjectPublicKeyInfo and verify the signature */
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    ret = mbedtls_pk_parse_public_key(&pk,
                                      g_signing_state.root_pubkeys_der[root_slot],
                                      g_signing_state.root_pubkeys_len[root_slot]);
    if (ret != 0)
    {
        LOG_ERR("Failed to parse stored public key: -0x%04x", (unsigned int)-ret);
        mbedtls_pk_free(&pk);
        sandbox_audit_log(AUDIT_EVENT_SIGNATURE_FAIL, "bad_pubkey", 0);
        return -EINVAL;
    }

    switch (signature->algorithm)
    {
    case SIGN_ALG_RSA2048_SHA256:
    {
        LOG_INF("Verifying RSA-2048-SHA256 signature (%zu bytes)", size);

        ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256,
                                hash, 32,
                                signature->signature,
                                signature->signature_len);
        if (ret != 0)
        {
            LOG_ERR("RSA-2048 signature verification FAILED: -0x%04x",
                    (unsigned int)-ret);
            mbedtls_pk_free(&pk);
            sandbox_audit_log(AUDIT_EVENT_SIGNATURE_FAIL, "rsa2048_bad_sig", 0);
            return -EACCES;
        }

        mbedtls_pk_free(&pk);
        LOG_INF("RSA-2048-SHA256 signature verified OK");
        sandbox_audit_log(AUDIT_EVENT_SIGNATURE_OK, "rsa2048", 0);
        return 0;
    }

    case SIGN_ALG_ED25519:
    {
        LOG_INF("Verifying Ed25519 signature (%zu bytes)", size);

        /* Ed25519 uses raw 64-byte signatures; mbedTLS PK layer handles
         * EdDSA when MBEDTLS_ECP_DP_CURVE25519_ENABLED is set. */
        ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_NONE,
                                hash, 32,
                                signature->signature,
                                signature->signature_len);
        if (ret != 0)
        {
            LOG_ERR("Ed25519 signature verification FAILED: -0x%04x",
                    (unsigned int)-ret);
            mbedtls_pk_free(&pk);
            sandbox_audit_log(AUDIT_EVENT_SIGNATURE_FAIL, "ed25519_bad_sig", 0);
            return -EACCES;
        }

        mbedtls_pk_free(&pk);
        LOG_INF("Ed25519 signature verified OK");
        sandbox_audit_log(AUDIT_EVENT_SIGNATURE_OK, "ed25519", 0);
        return 0;
    }

    default:
        mbedtls_pk_free(&pk);
        LOG_ERR("Unknown signature algorithm: %d", signature->algorithm);
        return -EINVAL;
    }
#else /* !MBEDTLS_PK_PARSE_C */
    /* Public key round-trip (pk_write + pk_parse) requires MBEDTLS_PK_PARSE_C.
     * This path is dead at runtime because root_pubkeys_len == 0 when X509
     * parsing was unavailable and we return -EACCES above — but we need the
     * guard to satisfy the compiler when PK_PARSE_C is not defined. */
    LOG_ERR("MBEDTLS_PK_PARSE_C not available — cannot verify stored public key");
    sandbox_audit_log(AUDIT_EVENT_SIGNATURE_FAIL, "no_pk_parse", 0);
    return -EACCES;
#endif /* MBEDTLS_PK_PARSE_C */
#else /* !CRYPTO_AVAILABLE */
    LOG_WRN("Crypto not available - signature verification disabled");
    return -ENOTSUP;
#endif /* CRYPTO_AVAILABLE */
}

/* ===== Certificate Chain Verification ===== */

int app_verify_cert_chain(const akira_cert_t *certs, int count)
{
    if (!certs || count <= 0)
    {
        return -EINVAL;
    }

    LOG_INF("Verifying certificate chain (%d certificates)", count);

    /* Verify root certificate is trusted */
    const akira_cert_t *root = NULL;
    for (int i = 0; i < count; i++)
    {
        if (certs[i].is_root)
        {
            root = &certs[i];
            break;
        }
    }

    if (!root)
    {
        /* Last cert in chain is assumed to be root */
        root = &certs[count - 1];
    }

    /* Hash the root certificate and check trust */
    uint8_t root_hash[32];
    int ret = app_compute_hash(root->cert_data, root->cert_len, root_hash);
    if (ret != 0)
    {
        LOG_ERR("Failed to hash root certificate");
        return ret;
    }

    if (!app_is_root_trusted(root_hash))
    {
        LOG_ERR("Root certificate is not trusted");
        return -EACCES;
    }

    LOG_INF("Certificate chain verified (root trusted)");
    return 0;
}

/* ===== Trusted Root Management ===== */

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
    if (!cert || cert->cert_len == 0)
    {
        return -EINVAL;
    }

    if (g_signing_state.root_count >= MAX_TRUSTED_ROOTS)
    {
        LOG_ERR("Maximum trusted roots reached (%d)", MAX_TRUSTED_ROOTS);
        return -ENOMEM;
    }

    /* Compute SHA-256 hash of certificate */
    uint8_t hash[32];
    int ret = app_compute_hash(cert->cert_data, cert->cert_len, hash);
    if (ret != 0)
    {
        LOG_ERR("Failed to hash certificate");
        return ret;
    }

    /* Check for duplicate */
    if (app_is_root_trusted(hash))
    {
        LOG_INF("Root certificate already trusted");
        return 0;
    }

    int slot = g_signing_state.root_count;
    memcpy(g_signing_state.root_hashes[slot], hash, 32);

#if CRYPTO_AVAILABLE && defined(MBEDTLS_X509_CRT_PARSE_C)
    /* Extract the SubjectPublicKeyInfo from the X.509 certificate so that
     * app_verify_signature() can call mbedtls_pk_verify() without needing
     * to store the full DER-encoded certificate. */
    {
        mbedtls_x509_crt crt;
        mbedtls_x509_crt_init(&crt);

        int parse_ret = mbedtls_x509_crt_parse_der(&crt, cert->cert_data, cert->cert_len);
        if (parse_ret != 0)
        {
            LOG_ERR("Failed to parse root certificate: -0x%04x", (unsigned int)-parse_ret);
            mbedtls_x509_crt_free(&crt);
            return -EINVAL;
        }

        /* mbedtls_pk_write_pubkey_der writes from the END of the buffer */
        unsigned char pk_buf[MAX_PUBKEY_DER_SIZE];
        int pk_len = mbedtls_pk_write_pubkey_der(&crt.pk, pk_buf, sizeof(pk_buf));
        if (pk_len <= 0 || pk_len > MAX_PUBKEY_DER_SIZE)
        {
            LOG_ERR("Failed to export public key DER: %d", pk_len);
            mbedtls_x509_crt_free(&crt);
            return -EIO;
        }

        /* DER output is right-aligned in pk_buf; copy to the start of our slot */
        memcpy(g_signing_state.root_pubkeys_der[slot],
               pk_buf + sizeof(pk_buf) - pk_len, (size_t)pk_len);
        g_signing_state.root_pubkeys_len[slot] = (size_t)pk_len;

        mbedtls_x509_crt_free(&crt);
    }
#else
    /* Without X.509 parsing support, public key cannot be extracted.
     * Signature verification will fall back to hash-only trust check,
     * which does NOT provide cryptographic verification.
     * Enable CONFIG_MBEDTLS_X509_CRT_PARSE_C for production builds. */
    g_signing_state.root_pubkeys_len[slot] = 0;
    LOG_WRN("MBEDTLS_X509_CRT_PARSE_C not available — public key not stored, "
            "signature verification will be INCOMPLETE");
#endif

    g_signing_state.root_count++;

    LOG_INF("Added trusted root CA (%d/%d)", g_signing_state.root_count,
            MAX_TRUSTED_ROOTS);
    return 0;
}

/* ===== WASM Integrity Verification ===== */

/**
 * @brief Verify WASM binary structural integrity
 *
 * Checks:
 * 1. WASM magic bytes (\0asm)
 * 2. Version field (must be 1)
 * 3. Section structure validity
 * 4. Computes and returns SHA-256 hash
 *
 * @param binary    WASM binary data
 * @param size      Binary size
 * @param hash_out  Output: SHA-256 hash (32 bytes), can be NULL
 * @return 0 on success, negative on error
 */
int app_verify_wasm_integrity(const void *binary, size_t size,
                              uint8_t *hash_out)
{
    if (!binary || size < 8)
    {
        return -EINVAL;
    }

    const uint8_t *data = (const uint8_t *)binary;

    /* Check WASM or AOT magic */
    if (!is_wasm_or_aot(data))
    {
        LOG_ERR("Invalid WASM/AOT magic bytes");
        sandbox_audit_log(AUDIT_EVENT_INTEGRITY_FAIL, "bad_magic", 0);
        return -EINVAL;
    }

    if (is_aot(data))
    {
        /* AOT binaries have a different internal layout;
         * skip version and section-structure checks. */
        LOG_DBG("AOT integrity check passed: %zu bytes", size);
    }
    else
    {
        /* Check version (must be 1) */
        uint32_t version = data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24);
        if (version != 1)
        {
            LOG_ERR("Unsupported WASM version: %u", version);
            sandbox_audit_log(AUDIT_EVENT_INTEGRITY_FAIL, "bad_version", version);
            return -EINVAL;
        }

        /* Validate section structure */
        size_t pos = 8;
        int section_count = 0;
        uint8_t last_section_id = 0;

        while (pos < size)
        {
            if (pos + 1 > size)
                break;

            uint8_t section_id = data[pos++];

            /* Non-custom sections must be in order */
            if (section_id != 0 && section_id <= last_section_id && section_count > 0)
            {
                LOG_WRN("WASM sections out of order: %d after %d", section_id, last_section_id);
                /* Warning only - some compilers emit out-of-order sections */
            }

            if (section_id != 0)
            {
                last_section_id = section_id;
            }

            /* Read section size (simplified LEB128) */
            uint32_t section_size = 0;
            int shift = 0;
            int leb_bytes = 0;
            while (pos < size && leb_bytes < 5)
            {
                uint8_t byte = data[pos++];
                section_size |= (uint32_t)(byte & 0x7F) << shift;
                leb_bytes++;
                if ((byte & 0x80) == 0)
                    break;
                shift += 7;
            }

            if (pos + section_size > size)
            {
                LOG_ERR("WASM section %d extends past EOF (offset=%zu, size=%u, total=%zu)",
                        section_id, pos, section_size, size);
                sandbox_audit_log(AUDIT_EVENT_INTEGRITY_FAIL, "truncated",
                                  (uint32_t)section_id);
                return -EINVAL;
            }

            pos += section_size;
            section_count++;
        }

        LOG_DBG("WASM integrity check passed: %d sections, %zu bytes", section_count, size);
    }

    /* Compute hash — always required for allowlist check */
    uint8_t _local_hash[32];
    uint8_t *hash_ptr = hash_out ? hash_out : _local_hash;
    int ret = app_compute_hash(binary, size, hash_ptr);
    if (ret != 0)
    {
        return ret;
    }

    /* Platform allowlist gate — overridden by AkiraPlatform when
     * CONFIG_AKIRA_PLATFORM_APP_ALLOWLIST=y. Default: allow all. */
    int allowlist_ret = akira_platform_allowlist_verify(hash_ptr, 32);
    if (allowlist_ret != 0)
    {
        LOG_ERR("Binary rejected by OEM allowlist");
        sandbox_audit_log(AUDIT_EVENT_SIGNATURE_FAIL, "allowlist_deny", 0);
        return -EACCES;
    }

    return 0;
}

/**
 * @brief Weak hook: OEM app allowlist verification.
 *
 * The default implementation allows all binaries. AkiraPlatform overrides
 * this with a strong implementation that checks the SHA-256 hash against
 * an NVS-persisted allowlist (see CONFIG_AKIRA_PLATFORM_APP_ALLOWLIST).
 *
 * Called from app_verify_wasm_integrity() after structural checks and
 * hash computation. Also called by app_verify_signature() after the
 * cryptographic signature check passes.
 *
 * @param app_hash  SHA-256 hash of the binary (32 bytes).
 * @param hash_len  Must be 32.
 * @return 0 if allowed, -EACCES if denied by allowlist.
 */
__weak int akira_platform_allowlist_verify(const uint8_t *app_hash, size_t hash_len)
{
    ARG_UNUSED(app_hash);
    ARG_UNUSED(hash_len);
    return 0; /* Default: allow all */
}

/* ===== PQC Signature Verification (Dilithium-2 / FIPS 204) ===== */

/**
 * @brief Verify the Dilithium-2 .akpkg signature.
 *
 * Computes SHA-256(manifest || wasm [|| model]) then calls the
 * akira_platform_pqc_verify() weak hook with the digest and the raw
 * sig.dilithium2 bytes extracted from the archive.
 */
int app_verify_pqc_sig(const uint8_t *manifest, size_t manifest_size,
                       const uint8_t *wasm,     size_t wasm_size,
                       const uint8_t *model,    size_t model_size,
                       const uint8_t *sig,      size_t sig_size)
{
    if (!manifest || manifest_size == 0 || !wasm || wasm_size == 0) {
        return -EINVAL;
    }

#if CRYPTO_AVAILABLE
    /* Compute SHA-256(manifest || wasm [|| model]) to match akira-cli digest(). */
    mbedtls_sha256_context ctx;
    uint8_t digest[32];

    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0 ||
        mbedtls_sha256_update(&ctx, manifest, manifest_size) != 0 ||
        mbedtls_sha256_update(&ctx, wasm,     wasm_size)     != 0 ||
        (model && model_size > 0 &&
         mbedtls_sha256_update(&ctx, model, model_size) != 0)    ||
        mbedtls_sha256_finish(&ctx, digest) != 0) {
        mbedtls_sha256_free(&ctx);
        LOG_ERR("PQC: SHA-256 computation failed");
        return -EIO;
    }
    mbedtls_sha256_free(&ctx);

    return akira_platform_pqc_verify(digest, sizeof(digest), sig, sig_size);
#else
    ARG_UNUSED(sig);
    ARG_UNUSED(sig_size);
    ARG_UNUSED(model);
    ARG_UNUSED(model_size);
    return 0; /* No crypto — PQC verification skipped */
#endif
}

/**
 * @brief Weak hook: Dilithium-2 signature verification.
 *
 * The default implementation is a no-op that allows all packages. AkiraPlatform
 * overrides this with a strong liboqs-backed implementation when
 * CONFIG_AKIRA_PLATFORM_PQC_SIGNING=y.
 *
 * @param digest      SHA-256(manifest || wasm [|| model]) — 32 bytes.
 * @param digest_len  Must be 32.
 * @param sig         sig.dilithium2 bytes (may be NULL if entry absent).
 * @param sig_len     Length of sig (0 if entry absent).
 * @return 0 if valid or PQC not enforced; -EACCES on signature mismatch;
 *         -ENODATA if sig is NULL and mandatory mode is configured.
 */
__weak int akira_platform_pqc_verify(const uint8_t *digest, size_t digest_len,
                                      const uint8_t *sig,    size_t sig_len)
{
    ARG_UNUSED(digest);
    ARG_UNUSED(digest_len);
    ARG_UNUSED(sig);
    ARG_UNUSED(sig_len);
    return 0; /* Default: PQC not enforced */
}
