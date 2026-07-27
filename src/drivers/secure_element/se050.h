/**
 * @file se050.h
 * @brief NXP EdgeLock SE050 Plug & Trust secure element — high-level API.
 *
 * Sits on top of the T=1' link layer (se050_transport.h) and speaks the SE05x
 * APDU/TLV applet protocol. Exposes the operations AkiraConsole needs:
 *   - true random (on-die TRNG)
 *   - secure binary object storage (readable, policy-protected)
 *   - P-256 (secp256r1) key generation, public-key read, ECDSA signing, with
 *     the private key generated on-die and never exportable
 *
 * All functions are thread-safe: the driver serialises access to the single
 * physical part with an internal mutex.
 *
 * SCOPE: the SE05x APDU constants track the public SE05x APDU specification.
 * The layer compiles and is structurally complete; exact enum values and the
 * live APDU exchange must be confirmed against the part before field use.
 * SCP03 (authenticated channel) is compiled only when CONFIG_AKIRA_SE050_SCP03.
 *
 * @stability experimental
 * @since 1.7
 */

#ifndef AKIRA_SE050_H
#define AKIRA_SE050_H

#include <stdint.h>
#include <stddef.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Uncompressed P-256 public key length (0x04 || X(32) || Y(32)). */
#define SE050_P256_PUB_LEN   65
/** Raw ECDSA P-256 signature length (r(32) || s(32)). */
#define SE050_P256_SIG_LEN   64

/**
 * @brief Get the SE050 device bound from device tree (compatible "nxp,se050").
 * @return Device pointer, or NULL if not present/enabled.
 */
const struct device *se050_get_device(void);

/** @brief Power the SE050 on (ENA high) and ensure the link is reset. */
int se050_power_on(const struct device *dev);

/** @brief Power the SE050 off (ENA low, deep power-down). */
int se050_power_off(const struct device *dev);

/**
 * @brief Fill @p buf with @p len bytes from the SE050 hardware TRNG.
 * @return 0 on success, negative errno otherwise.
 */
int se050_get_random(const struct device *dev, uint8_t *buf, size_t len);

/**
 * @brief Write a secure binary object (create-or-overwrite).
 * @param objid  32-bit SE050 object identifier.
 * @param data   Bytes to store.
 * @param len    Number of bytes.
 * @return 0 on success, negative errno otherwise.
 */
int se050_write_binary(const struct device *dev, uint32_t objid,
                       const uint8_t *data, size_t len);

/**
 * @brief Read a secure binary object.
 * @param objid    32-bit SE050 object identifier.
 * @param buf      Destination buffer.
 * @param cap      Capacity of @p buf.
 * @param out_len  Out: bytes read.
 * @return 0 on success, -ENOENT if the object does not exist, else negative errno.
 */
int se050_read_binary(const struct device *dev, uint32_t objid,
                      uint8_t *buf, size_t cap, size_t *out_len);

/** @brief True if a secure object with @p objid exists. */
bool se050_object_exists(const struct device *dev, uint32_t objid);

/**
 * @brief Generate a P-256 key pair on-die under @p objid (private key is
 *        non-exportable). No-op success if the object already exists.
 */
int se050_ecc_gen_key(const struct device *dev, uint32_t objid);

/**
 * @brief Read the 65-byte uncompressed public key of P-256 object @p objid.
 */
int se050_ecc_get_pub(const struct device *dev, uint32_t objid,
                     uint8_t pub[SE050_P256_PUB_LEN]);

/**
 * @brief ECDSA/SHA-256 sign a 32-byte digest with P-256 object @p objid.
 * @param hash    32-byte message digest.
 * @param sig_rs  Output 64-byte raw signature (r||s), not DER.
 */
int se050_ecc_sign(const struct device *dev, uint32_t objid,
                  const uint8_t hash[32], uint8_t sig_rs[SE050_P256_SIG_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_SE050_H */
