/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#include <akira_identity.h>
#include "akira.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <inttypes.h>
#include <string.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif
#if defined(CONFIG_BOOTLOADER_MCUBOOT) && defined(CONFIG_FLASH_MAP)
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#endif
#ifdef CONFIG_HWINFO
#include <zephyr/drivers/hwinfo.h>
#endif

/* Set by zephyr/CMakeLists.txt from `git describe` at configure time. */
#ifndef AKIRA_BUILD_REVISION
#define AKIRA_BUILD_REVISION "unknown"
#endif
#ifndef AKIRA_WAMR_REVISION
#define AKIRA_WAMR_REVISION "unknown"
#endif

void akira_identity_get(struct akira_identity *id)
{
	memset(id, 0, sizeof(*id));
	id->akira_version = AKIRA_VERSION_STRING;
	id->akira_revision = AKIRA_BUILD_REVISION;
	id->zephyr_version = STRINGIFY(BUILD_VERSION);
	id->board = CONFIG_BOARD_TARGET;
	id->wamr_revision = "";

#ifdef CONFIG_AKIRA_WASM_RUNTIME
	uint32_t major, minor, patch;

	wasm_runtime_get_version(&major, &minor, &patch);
	snprintk(id->wamr_version, sizeof(id->wamr_version), "%u.%u.%u",
		 major, minor, patch);
	id->wamr_revision = AKIRA_WAMR_REVISION;
#endif

#if defined(CONFIG_BOOTLOADER_MCUBOOT) && defined(CONFIG_FLASH_MAP)
	/* The version MCUboot verified (signed header), not a compiled-in string. */
	struct mcuboot_img_header hdr;

	if (boot_read_bank_header(FIXED_PARTITION_ID(slot0_partition), &hdr,
				  sizeof(hdr)) == 0) {
		snprintk(id->image_version, sizeof(id->image_version),
			 "%u.%u.%u+%" PRIu32, hdr.h.v1.sem_ver.major,
			 hdr.h.v1.sem_ver.minor, hdr.h.v1.sem_ver.revision,
			 hdr.h.v1.sem_ver.build_num);
	}
#endif

#ifdef CONFIG_HWINFO
	uint8_t raw[16];
	ssize_t n = hwinfo_get_device_id(raw, sizeof(raw));

	for (ssize_t i = 0; i < n; i++) {
		snprintk(&id->device_id[2 * i], 3, "%02x", raw[i]);
	}
#endif
}
