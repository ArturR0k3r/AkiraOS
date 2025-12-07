/**
 * @file app_signing.h
 * @brief AkiraOS App Signing and Verification
 */

#ifndef AKIRA_APP_SIGNING_H
#define AKIRA_APP_SIGNING_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Signature algorithm types
     */
    typedef enum
    {
        SIGN_ALG_NONE = 0,
        SIGN_ALG_RSA2048_SHA256,
        SIGN_ALG_ED25519
    } akira_sign_alg_t;

    /**
     * @brief App signature structure
     */
    typedef struct
    {
        akira_sign_alg_t algorithm;
        uint8_t signature[256]; // Max size for RSA-2048
        size_t signature_len;
        uint8_t cert_hash[32]; // SHA-256 of signing cert
    } akira_app_signature_t;

    /**
     * @brief Certificate chain entry
     */
    typedef struct
    {
        uint8_t cert_data[1024]; // DER-encoded certificate
        size_t cert_len;
        bool is_root;
    } akira_cert_t;

    /**
     * @brief Initialize signing subsystem
     * @return 0 on success
     */
    int app_signing_init(void);

    /**
     * @brief Verify app binary signature
     * @param binary WASM binary data
     * @param size Binary size
     * @param signature Signature to verify
     * @return 0 if valid, negative on error
     */
    int app_verify_signature(const void *binary, size_t size,
                             const akira_app_signature_t *signature);

    /**
     * @brief Verify certificate chain
     * @param certs Certificate chain (leaf to root)
     * @param count Number of certificates
     * @return 0 if valid, negative on error
     */
    int app_verify_cert_chain(const akira_cert_t *certs, int count);

    /**
     * @brief Check if root CA is trusted
     * @param cert_hash SHA-256 hash of root certificate
     * @return true if trusted
     */
    bool app_is_root_trusted(const uint8_t *cert_hash);

    /**
     * @brief Add trusted root CA
     * @param cert Root certificate
     * @return 0 on success
     */
    int app_add_trusted_root(const akira_cert_t *cert);

    /**
     * @brief Compute SHA-256 hash of data
     * @param data Input data
     * @param len Data length
     * @param hash Output hash (32 bytes)
     * @return 0 on success
     */
    int app_compute_hash(const void *data, size_t len, uint8_t *hash);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_APP_SIGNING_H */
