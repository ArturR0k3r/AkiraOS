/**
 * @file akira_ed25519.h
 * @brief Ed25519 (RFC 8032, PureEdDSA) signature verification
 *
 * mbedTLS 3.x has no EdDSA, so AkiraOS carries the fiat-crypto Ed25519
 * verifier that MCUboot also uses (src/lib/crypto/ed25519, MIT licence).
 * Signatures made by Go's crypto/ed25519, OpenSSL, libsodium or PyCA
 * cryptography verify here; the message is hashed with SHA-512 internally.
 *
 * Verification only: no signing, no key generation.
 *
 * @stability experimental
 * @since 1.7
 */

#ifndef AKIRA_ED25519_H
#define AKIRA_ED25519_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AKIRA_ED25519_PUBKEY_LEN 32
#define AKIRA_ED25519_SIG_LEN    64

/**
 * @brief Verify an Ed25519 signature.
 *
 * @param msg     Signed message (any length; may be NULL when @p len is 0).
 * @param len     Message length in bytes.
 * @param sig     64-byte signature (R || S).
 * @param pubkey  32-byte public key.
 * @return 0 if the signature is valid, -EBADMSG if it is not, -EINVAL on a
 *         NULL argument.
 */
int akira_ed25519_verify(const uint8_t *msg, size_t len,
                         const uint8_t sig[AKIRA_ED25519_SIG_LEN],
                         const uint8_t pubkey[AKIRA_ED25519_PUBKEY_LEN]);

/**
 * @brief Parse a 64-character hex string into a 32-byte Ed25519 public key.
 *
 * @return 0 on success, -EINVAL if @p hex is not exactly 64 hex digits.
 */
int akira_ed25519_pubkey_from_hex(const char *hex,
                                  uint8_t out[AKIRA_ED25519_PUBKEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_ED25519_H */
