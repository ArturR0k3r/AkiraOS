/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file atecc608b.h
 * @brief Microchip ATECC608B CryptoAuthentication secure element.
 *
 * A hardware root of trust on I2C: P-256 private keys generated inside the
 * part that cannot be read out, HMAC keys usable only inside it, a
 * factory-programmed 9-byte serial number and a hardware TRNG.
 *
 * Origin: the Latch product firmware (latch-fw), upstreamed in 1.7 and
 * extended with key generation, signing, HMAC, data writes and zone locking.
 *
 * Hardware status: wake, Info, serial, Random and lock-state reads come from
 * latch-fw. GenKey, Nonce/Sign, SHA-HMAC, Write and Lock follow the ATECC608B
 * datasheet command set and are not yet validated on a part. Configuration
 * and lock operations are irreversible; run them only from a provisioning
 * flow that has been checked on a sacrificial part.
 *
 * Every operation wakes the part and returns it to sleep, so it never idles
 * at ~800 µA. All calls are serialised by an internal mutex and may block for
 * up to ~150 ms (key generation).
 *
 * Include as <drivers/secure_element/atecc608b.h>.
 *
 * @stability experimental
 * @since 1.7
 */

#ifndef AKIRA_ATECC608B_H
#define AKIRA_ATECC608B_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Length of the factory-programmed serial number, in bytes. */
#define ATECC608B_SERIAL_LEN 9

/** Maximum bytes one RNG command returns. */
#define ATECC608B_RANDOM_LEN 32

/** Size of the configuration zone. */
#define ATECC608B_CONFIG_LEN 128

/** P-256 public key as the part returns it: X || Y, 32 bytes each. */
#define ATECC608B_PUBKEY_LEN 64

/** Raw ECDSA signature r || s. */
#define ATECC608B_SIG_LEN 64

/** Number of key slots. */
#define ATECC608B_SLOTS 16

/**
 * @brief Probe and initialise the secure element.
 *
 * Wakes the part, reads its revision, and caches the serial number. Called
 * automatically at boot via SYS_INIT. Idempotent.
 *
 * @return 0 on success, -ENODEV if the part does not answer, or a negative
 *         errno from the I2C layer.
 */
int atecc608b_init(void);

/** @return true once the part has answered a wake and revision read. */
bool atecc608b_is_present(void);

/** Read the device revision word (Info command, Revision mode). */
int atecc608b_get_revision(uint32_t *revision);

/** Copy the cached 9-byte factory serial number. */
int atecc608b_get_serial(uint8_t *out);

/**
 * @brief Read the config and data zone lock state.
 *
 * A part with both zones unlocked is blank: its keys are not yet generated and
 * nothing it signs means anything.
 */
int atecc608b_get_lock_state(bool *config_locked, bool *data_locked);

/**
 * @brief Draw bytes from the part's hardware TRNG.
 * @return Number of bytes written (1..ATECC608B_RANDOM_LEN), or negative errno.
 */
int atecc608b_random(uint8_t *out, size_t len);

/**
 * @brief Generate a new P-256 key pair in @p slot (GenKey, create mode).
 *
 * The slot must be configured as a private-key slot that permits GenKey.
 * @param pubkey  Receives the 64-byte public key, may be NULL.
 */
int atecc608b_genkey(uint8_t slot, uint8_t pubkey[ATECC608B_PUBKEY_LEN]);

/** Compute the public key of the private key in @p slot (GenKey, public mode). */
int atecc608b_get_pubkey(uint8_t slot, uint8_t pubkey[ATECC608B_PUBKEY_LEN]);

/**
 * @brief ECDSA-sign a 32-byte digest with the private key in @p slot.
 *
 * Loads the digest into TempKey (Nonce, pass-through) and runs Sign in
 * external-message mode within one wake.
 */
int atecc608b_sign(uint8_t slot, const uint8_t digest[32],
		   uint8_t sig[ATECC608B_SIG_LEN]);

/**
 * @brief HMAC-SHA256 of @p data with the symmetric key in @p slot,
 *        computed on the part (SHA command, HMAC mode).
 */
int atecc608b_hmac_sha256(uint8_t slot, const uint8_t *data, size_t len,
			  uint8_t mac[32]);

/**
 * @brief Write one 32-byte block of a data slot in clear text.
 *
 * Only possible while the data zone is unlocked, or later if the slot's
 * configuration allows clear writes. Use it at provisioning to load HMAC keys.
 */
int atecc608b_write_slot_block(uint8_t slot, uint8_t block, const uint8_t data[32]);

/**
 * @brief Write the configuration zone from a 128-byte image.
 *
 * Writes bytes 16..83 and 88..127 in 4-byte words (bytes 0..15 are read-only;
 * 84..87 hold UserExtra/Selector/lock bytes, which Write cannot change). The
 * config zone must be unlocked.
 */
int atecc608b_write_config(const uint8_t config[ATECC608B_CONFIG_LEN]);

/** Read the whole 128-byte configuration zone. */
int atecc608b_read_config(uint8_t config[ATECC608B_CONFIG_LEN]);

/** Lock the configuration zone. Irreversible. */
int atecc608b_lock_config(void);

/** Lock the data and OTP zones. Irreversible. */
int atecc608b_lock_data(void);

/** Lock one slot (its KeyConfig must have Lockable set). Irreversible. */
int atecc608b_lock_slot(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_ATECC608B_H */
