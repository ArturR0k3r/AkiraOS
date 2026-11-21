/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 *
 * include/akira_ota.h for builds without OTA support (no MCUboot, no flash
 * map, or CONFIG_AKIRA_OTA=n).
 */

#include <akira_ota.h>

#include <errno.h>
#include <zephyr/sys/util.h>

bool akira_ota_available(void)
{
	return false;
}

int akira_ota_begin(size_t image_size, const uint8_t sha256[32])
{
	ARG_UNUSED(image_size);
	ARG_UNUSED(sha256);
	return -ENOTSUP;
}

int akira_ota_write(const uint8_t *data, size_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return -ENOTSUP;
}

int akira_ota_finish(void)
{
	return -ENOTSUP;
}

int akira_ota_finish_ex(uint32_t flags)
{
	ARG_UNUSED(flags);
	return -ENOTSUP;
}

int akira_ota_abort(void)
{
	return -ENOTSUP;
}

void akira_ota_reboot(uint32_t delay_ms)
{
	ARG_UNUSED(delay_ms);
}
