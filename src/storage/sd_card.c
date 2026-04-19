/*
 * Copyright (c) 2026 PenEngineering S.R.L
* SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_sd_card
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_sd_card, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file sd_card.c
 * @brief SD card hardware init — mounts FATFS at /SD: via SYS_INIT.
 *
 * Hardware-agnostic: the actual SPI-SDHC or native-SDMMC driver is selected
 * via each board's .conf and .overlay.  This module only performs disk probe,
 * FATFS mount (FS_FATFS), and /SD:/apps directory creation.
 *
 * Runs at APPLICATION level, CONFIG_AKIRA_SD_INIT_PRIORITY (default 38),
 * so the SD card is ready before fs_manager (priority 40) queries it.
 */

#ifdef CONFIG_AKIRA_SD_CARD

#include "sd_card.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <ff.h>
#include <errno.h>

/* Disk name must match `disk-name` in the DTS mmc{} node */
#define SD_DISK_NAME   "SD"
#define SD_MOUNT_POINT "/SD:"
#define SD_APPS_DIR    "/SD:/apps"

static bool  g_mounted;
static FATFS g_fat_fs;

static struct fs_mount_t g_sd_mount = {
    .type      = FS_FATFS,
    .fs_data   = &g_fat_fs,
    .mnt_point = SD_MOUNT_POINT,
};

int akira_sd_card_init(void)
{
    if (g_mounted) {
        return 0;
    }

    /* Probe the disk layer — returns 0 if the card is present */
    int ret = disk_access_init(SD_DISK_NAME);
    if (ret < 0) {
        LOG_DBG("SD disk \"%s\" not found: %d", SD_DISK_NAME, ret);
        return ret;
    }

    if (disk_access_status(SD_DISK_NAME) != DISK_STATUS_OK) {
        LOG_DBG("SD disk status error");
        return -EIO;
    }

    /* Mount FATFS */
    ret = fs_mount(&g_sd_mount);
    if (ret < 0) {
        LOG_WRN("SD mount failed: %d", ret);
        return ret;
    }

    g_mounted = true;

    /* Ensure /SD:/apps exists for app installs */
    (void)fs_mkdir(SD_APPS_DIR);

    LOG_INF("SD card mounted at %s", SD_MOUNT_POINT);
    return 0;
}

bool akira_sd_card_is_present(void)
{
    return g_mounted;
}

void akira_sd_card_deinit(void)
{
    if (!g_mounted) {
        return;
    }

    int ret = fs_unmount(&g_sd_mount);
    if (ret < 0) {
        LOG_WRN("SD unmount failed: %d", ret);
        return;
    }

    g_mounted = false;
    LOG_INF("SD card unmounted");
}

/* Init before fs_manager (APPLICATION, priority 40) so sd_available is set */
SYS_INIT(akira_sd_card_init, APPLICATION, CONFIG_AKIRA_SD_INIT_PRIORITY);

#endif /* CONFIG_AKIRA_SD_CARD */
