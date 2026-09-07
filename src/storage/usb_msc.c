/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_usb_msc
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_usb_msc, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file usb_msc.c
 * @brief USB Mass Storage ownership shim for the SD card.
 *
 * usbd_msc/scsi.c has no runtime "LUN not ready" API — the only lever is a
 * disk's disk_access_status(). This registers a proxy disk ("AKIRA_SD")
 * that forwards read/write/ioctl straight to the real "SD" disk, but
 * reports not-ready until AkiraOS has actually released the card.
 */

#include "usb_msc.h"

#include <zephyr/drivers/disk.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <errno.h>

#include "fs_manager.h"
#include "sd_card.h"
#include "../connectivity/usb/usb_manager.h"

#define AKIRA_MSC_SD_DISK_NAME "AKIRA_SD"
#define AKIRA_MSC_REAL_SD_DISK_NAME "SD"
#define AKIRA_MSC_POLL_INTERVAL_MS 200

USBD_DEFINE_MSC_LUN(sd, AKIRA_MSC_SD_DISK_NAME, "AkiraOS", "SD Card", "1.0");

static bool g_msc_owns_sd;
static bool g_enter_cancel_requested;

#define AKIRA_USB_MSC_MAX_CBS 2
static struct {
    akira_usb_msc_state_cb_t cb;
    void                     *user_data;
} g_state_cbs[AKIRA_USB_MSC_MAX_CBS];

bool akira_usb_msc_owns_sd(void)
{
    return g_msc_owns_sd;
}

int akira_usb_msc_register_state_cb(akira_usb_msc_state_cb_t cb, void *user_data)
{
    for (int i = 0; i < AKIRA_USB_MSC_MAX_CBS; i++) {
        if (!g_state_cbs[i].cb) {
            g_state_cbs[i].cb        = cb;
            g_state_cbs[i].user_data = user_data;
            return 0;
        }
    }
    LOG_WRN("USB MSC: no free state callback slots");
    return -ENOMEM;
}

static void notify_state_cbs(bool owns_sd)
{
    for (int i = 0; i < AKIRA_USB_MSC_MAX_CBS; i++) {
        if (g_state_cbs[i].cb) {
            g_state_cbs[i].cb(owns_sd, g_state_cbs[i].user_data);
        }
    }
}

static int shim_init(struct disk_info *disk)
{
    ARG_UNUSED(disk);
    return disk_access_init(AKIRA_MSC_REAL_SD_DISK_NAME);
}

static int shim_status(struct disk_info *disk)
{
    ARG_UNUSED(disk);
    if (!g_msc_owns_sd) {
        return DISK_STATUS_UNINIT;
    }
    /* The real disk's status() only discovers a physical pull on a failed
     * transaction (SDMMC timeout, ~600ms) — the TCA6408 detect pin knows
     * immediately, so check it first and skip straight to NOMEDIA instead
     * of paying that timeout on every host poll. */
    if (!akira_sd_card_is_physically_present()) {
        return DISK_STATUS_NOMEDIA;
    }
    return disk_access_status(AKIRA_MSC_REAL_SD_DISK_NAME);
}

static int shim_read(struct disk_info *disk, uint8_t *data_buf,
                     uint32_t start_sector, uint32_t num_sector)
{
    ARG_UNUSED(disk);
    if (!akira_sd_card_is_physically_present()) {
        return -ENODEV;
    }
    return disk_access_read(AKIRA_MSC_REAL_SD_DISK_NAME, data_buf,
                            start_sector, num_sector);
}

static int shim_write(struct disk_info *disk, const uint8_t *data_buf,
                      uint32_t start_sector, uint32_t num_sector)
{
    ARG_UNUSED(disk);
    if (!akira_sd_card_is_physically_present()) {
        return -ENODEV;
    }
    return disk_access_write(AKIRA_MSC_REAL_SD_DISK_NAME, data_buf,
                             start_sector, num_sector);
}

static int shim_ioctl(struct disk_info *disk, uint8_t cmd, void *buf)
{
    ARG_UNUSED(disk);
    return disk_access_ioctl(AKIRA_MSC_REAL_SD_DISK_NAME, cmd, buf);
}

static const struct disk_operations shim_ops = {
    .init = shim_init,
    .status = shim_status,
    .read = shim_read,
    .write = shim_write,
    .ioctl = shim_ioctl,
};

static struct disk_info shim_disk = {
    .name = AKIRA_MSC_SD_DISK_NAME,
    .ops = &shim_ops,
};

static int akira_usb_msc_shim_init(void)
{
    return disk_access_register(&shim_disk);
}

SYS_INIT(akira_usb_msc_shim_init, APPLICATION, CONFIG_AKIRA_USB_MSC_SHIM_INIT_PRIORITY);

int akira_usb_msc_enter(void)
{
    int64_t deadline;

    g_enter_cancel_requested = false;
    deadline = k_uptime_get() + CONFIG_AKIRA_USB_MSC_WAIT_TIMEOUT_MS;

    while (akira_sd_card_is_transfer_active()) {
        if (g_enter_cancel_requested) {
            LOG_INF("USB MSC enter: aborted while waiting for SD");
            return -ECANCELED;
        }
        if (k_uptime_get() >= deadline) {
            LOG_WRN("USB MSC enter: timed out waiting for SD to free up");
            return -ETIMEDOUT;
        }
        k_sleep(K_MSEC(AKIRA_MSC_POLL_INTERVAL_MS));
    }

    akira_sd_card_release_for_usb_msc();
    fs_manager_set_sd_available(false);
    g_msc_owns_sd = true;
    notify_state_cbs(true);

    return usb_manager_activate(USB_MODE_MSC);
}

void akira_usb_msc_cancel_enter(void)
{
    g_enter_cancel_requested = true;
}

void akira_usb_msc_exit(void)
{
    g_msc_owns_sd = false;
    usb_manager_activate(USB_MODE_IDLE);
    akira_sd_card_init();
    notify_state_cbs(false);
}
