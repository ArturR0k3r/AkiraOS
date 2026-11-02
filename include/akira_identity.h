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
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_IDENTITY_H
#define AKIRA_IDENTITY_H

#ifdef __cplusplus
extern "C" {
#endif

/** Platform parts and versions. Empty strings mean "not in this build". */
struct akira_identity {
	const char *akira_version;  /**< AkiraOS VERSION file, e.g. "1.6.5" */
	const char *akira_revision; /**< `git describe` of AkiraOS at build time */
	const char *zephyr_version; /**< Zephyr BUILD_VERSION */
	const char *board;          /**< Zephyr board target */
	char wamr_version[16];      /**< WAMR release, e.g. "2.4.3" */
	const char *wamr_revision;  /**< `git describe` of the WAMR west project */
	char image_version[32];     /**< Running image's MCUboot header version */
	char device_id[33];         /**< Hardware device ID, hex */
};

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
