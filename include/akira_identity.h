/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_identity.h
 * @brief Identity of the running platform: every part and its version.
 *
 * One place that answers "what exactly is running here": the AkiraOS release
 * and source revision, the Zephyr and WAMR versions it was built with, the
 * version MCUboot verified in the running image's header, the board, and the
 * hardware device ID. `akira version` and GET /api/v1/info report it.
 *
 * Since 1.7 an identity provider (AKIRA_IDENTITY_PROVIDER_DEFINE) can add the
 * identity a device was given at manufacturing: a serial number, the owning
 * organisation and the fingerprint of its device certificate.
 *
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_IDENTITY_H
#define AKIRA_IDENTITY_H

#include <zephyr/sys/iterable_sections.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Identity assigned at manufacturing. Empty strings: not provisioned.
 * Providers must write printable ASCII without quotes or backslashes (the
 * fields are embedded verbatim in JSON). */
struct akira_identity_provisioned {
	char serial[33];        /**< device serial / certificate subject CN */
	char org[33];           /**< owning organisation */
	char cert_sha256[65];   /**< SHA-256 of the device certificate (DER), hex */
	char key_store[16];     /**< where the device key lives, e.g. "se050" */
};

/** Platform parts and versions. Empty strings mean "not in this build". */
struct akira_identity {
	const char *akira_version;  /**< AkiraOS VERSION file, e.g. "1.6.7" */
	const char *akira_revision; /**< `git describe` of AkiraOS at build time */
	const char *zephyr_version; /**< Zephyr BUILD_VERSION */
	const char *board;          /**< Zephyr board target */
	char wamr_version[16];      /**< WAMR release, e.g. "2.4.3" */
	const char *wamr_revision;  /**< `git describe` of the WAMR west project */
	char image_version[32];     /**< Running image's MCUboot header version */
	char device_id[33];         /**< Hardware device ID, hex */
	struct akira_identity_provisioned provisioned; /**< since 1.7 */
};

/**
 * Identity provider: fills `id->provisioned`. Called on every
 * akira_identity_get(), on the caller's thread; must not block on I/O (cache
 * what it reads at boot).
 */
typedef void (*akira_identity_provider_fn)(struct akira_identity_provisioned *out,
					   void *user_data);

/** One registered provider. Create with AKIRA_IDENTITY_PROVIDER_DEFINE(). */
struct akira_identity_provider {
	akira_identity_provider_fn fn;
	void *user_data;
};

/** Register an identity provider. Providers run in link order. */
#define AKIRA_IDENTITY_PROVIDER_DEFINE(id, fn, user)                               \
	const STRUCT_SECTION_ITERABLE(akira_identity_provider, id) = { (fn), (user) }

/**
 * @brief Fill @p id with the identity of the running platform.
 *
 * Reads the MCUboot header of the primary slot and the hardware ID on each
 * call; everything else is fixed at build time.
 */
void akira_identity_get(struct akira_identity *id);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_IDENTITY_H */
