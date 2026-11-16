/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public OTA façade (include/akira_ota.h) over ota_manager.
 */

#include <akira_ota.h>
#include "ota_manager.h"

#include <errno.h>

static int to_errno(enum ota_result r)
{
	switch (r) {
	case OTA_OK:
		return 0;
	case OTA_ERROR_INVALID_PARAM:
		return -EINVAL;
	case OTA_ERROR_NOT_INITIALIZED:
		return -ENODEV;
	case OTA_ERROR_ALREADY_IN_PROGRESS:
		return -EBUSY;
	case OTA_ERROR_INSUFFICIENT_SPACE:
		return -ENOSPC;
	case OTA_ERROR_INVALID_IMAGE:
	case OTA_ERROR_SIGNATURE_VERIFICATION:
		return -EBADMSG;
	case OTA_ERROR_TIMEOUT:
		return -ETIMEDOUT;
	default:
		return -EIO;
	}
}

bool akira_ota_available(void)
{
	return true;
}

int akira_ota_begin(size_t image_size, const uint8_t sha256[32])
{
	if (image_size == 0) {
		return -EINVAL;
	}
	int ret = to_errno(ota_start_update(image_size));

	if (ret == 0 && sha256) {
		ret = to_errno(ota_set_expected_sha256(sha256));
		if (ret != 0) {
			(void)ota_abort_update();
		}
	}
	return ret;
}

int akira_ota_write(const uint8_t *data, size_t len)
{
	/* ota_write_chunk() takes at most 64 KiB per call. */
	while (len > 0) {
		size_t n = len > 4096 ? 4096 : len;
		int ret = to_errno(ota_write_chunk(data, n));

		if (ret != 0) {
			(void)ota_abort_update();
			return ret;
		}
		data += n;
		len -= n;
	}
	return 0;
}

int akira_ota_finish(void)
{
	return to_errno(ota_finalize_update());
}

int akira_ota_abort(void)
{
	return to_errno(ota_abort_update());
}

void akira_ota_reboot(uint32_t delay_ms)
{
	ota_reboot_to_apply_update(delay_ms);
}
