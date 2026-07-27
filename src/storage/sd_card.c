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
 *
 * Hotplug (CONFIG_AKIRA_SD_HOTPLUG): polls a board-supplied "akira,sd-detect"
 * devicetree node via gpio_dt_spec, so the detect pin can live on any native
 * GPIO or expander without this file naming a controller. Heavy work (mount/
 * unmount, callback fan-out) runs on a dedicated work queue off the poller.
 */

#ifdef CONFIG_AKIRA_SD_CARD

#include "sd_card.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/drivers/disk.h>
#include <ff.h>
#include <errno.h>
#include <string.h>

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

/* ------------------------------------------------------------------ */
/* Hotplug                                                              */
/* ------------------------------------------------------------------ */
#ifdef CONFIG_AKIRA_SD_HOTPLUG

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#define SD_DETECT_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(akira_sd_detect)
static const struct gpio_dt_spec g_sd_det =
    GPIO_DT_SPEC_GET(SD_DETECT_NODE, detect_gpios);

typedef struct {
    akira_sd_hotplug_cb_t cb;
    void                 *user_data;
} hotplug_entry_t;

static hotplug_entry_t g_hotplug_cbs[CONFIG_AKIRA_SD_HOTPLUG_MAX_CBS];

/* Pre-insert callback — fires before akira_sd_card_init() so a UI layer can
 * show a loading indicator immediately when the card is detected. */
static akira_sd_hotplug_cb_t g_pre_insert_cb;
static void                 *g_pre_insert_user;
static int                   g_last_det = -1; /* -1 = unknown */
static bool                  g_event_pending;  /* guard against double-submit */

/* Dedicated work queue — fs_unmount + registry ops need headroom sysworkq
 * shouldn't spend on a poll-driven event. */
#define SD_EVENT_WQ_STACK 4096
#define SD_EVENT_WQ_PRIO   11

K_THREAD_STACK_DEFINE(g_sd_event_stack, SD_EVENT_WQ_STACK);
static struct k_work_q g_sd_event_wq;
static struct k_work   g_sd_event_work;
static bool             g_sd_event_present;

static void sd_event_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    bool present = g_sd_event_present;
    g_event_pending = false;

    if (present && !g_mounted) {
        LOG_INF("SD hotplug: card inserted");
        if (g_pre_insert_cb) {
            g_pre_insert_cb(true, g_pre_insert_user);
        }
        int ret = akira_sd_card_init();
        if (ret < 0) {
            LOG_ERR("SD hotplug mount failed: %d", ret);
            return;
        }
        for (int i = 0; i < CONFIG_AKIRA_SD_HOTPLUG_MAX_CBS; i++) {
            if (g_hotplug_cbs[i].cb) {
                g_hotplug_cbs[i].cb(true, g_hotplug_cbs[i].user_data);
            }
        }
    } else if (!present && g_mounted) {
        LOG_INF("SD hotplug: card removed");
        akira_sd_card_deinit_force();
        for (int i = 0; i < CONFIG_AKIRA_SD_HOTPLUG_MAX_CBS; i++) {
            if (g_hotplug_cbs[i].cb) {
                g_hotplug_cbs[i].cb(false, g_hotplug_cbs[i].user_data);
            }
        }
    }
}

/* Poll runs on sysworkq — cheap GPIO read, submits an event only on change */
static void poll_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_poll_work, poll_work_fn);

static void poll_work_fn(struct k_work *work)
{
    int val = gpio_pin_get_dt(&g_sd_det);
    if (val < 0) {
        LOG_ERR("SD detect read failed: %d", val);
        goto reschedule;
    }

    if (val != g_last_det && !g_event_pending) {
        g_last_det          = val;
        g_sd_event_present  = (val == 1);
        g_event_pending     = true;
        k_work_submit_to_queue(&g_sd_event_wq, &g_sd_event_work);
    }

reschedule:
    k_work_reschedule(k_work_delayable_from_work(work),
                      K_MSEC(CONFIG_AKIRA_SD_HOTPLUG_POLL_MS));
}

int akira_sd_card_register_hotplug_cb(akira_sd_hotplug_cb_t cb, void *user_data)
{
    for (int i = 0; i < CONFIG_AKIRA_SD_HOTPLUG_MAX_CBS; i++) {
        if (!g_hotplug_cbs[i].cb) {
            g_hotplug_cbs[i].cb        = cb;
            g_hotplug_cbs[i].user_data = user_data;
            return 0;
        }
    }
    LOG_WRN("SD hotplug: no free callback slots");
    return -ENOMEM;
}

void akira_sd_card_unregister_hotplug_cb(akira_sd_hotplug_cb_t cb)
{
    for (int i = 0; i < CONFIG_AKIRA_SD_HOTPLUG_MAX_CBS; i++) {
        if (g_hotplug_cbs[i].cb == cb) {
            g_hotplug_cbs[i].cb        = NULL;
            g_hotplug_cbs[i].user_data = NULL;
            return;
        }
    }
}

void akira_sd_card_register_pre_insert_cb(akira_sd_hotplug_cb_t cb, void *user_data)
{
    g_pre_insert_cb   = cb;
    g_pre_insert_user = user_data;
}

static int hotplug_init(void)
{
    if (!gpio_is_ready_dt(&g_sd_det)) {
        LOG_ERR("SD detect GPIO not ready");
        return -ENODEV;
    }

    int ret = gpio_pin_configure_dt(&g_sd_det, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("SD detect configure failed: %d", ret);
        return ret;
    }

    k_work_queue_start(&g_sd_event_wq, g_sd_event_stack,
                       K_THREAD_STACK_SIZEOF(g_sd_event_stack),
                       SD_EVENT_WQ_PRIO, NULL);
    k_thread_name_set(&g_sd_event_wq.thread, "sd_event");
    k_work_init(&g_sd_event_work, sd_event_work_fn);

    k_work_schedule(&g_poll_work, K_MSEC(CONFIG_AKIRA_SD_HOTPLUG_POLL_MS));
    LOG_INF("SD hotplug ready (%d ms poll)", CONFIG_AKIRA_SD_HOTPLUG_POLL_MS);
    return 0;
}

#endif /* CONFIG_AKIRA_SD_HOTPLUG */

int akira_sd_card_init(void)
{
    if (g_mounted) {
        return 0;
    }

    /* Fully deinit the disk controller before reinitializing.
     * Without this, disk_access_init fails after a card removal+reinsertion
     * because the SDMMC host driver still holds its previous session state.
     * DISK_IOCTL_CTRL_DEINIT is a no-op if the disk was never initialized. */
    disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_CTRL_DEINIT, NULL);

    /* Probe the disk layer — retry a few times to let the card stabilize */
    int ret = -EIO;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            k_msleep(200);
        }
        ret = disk_access_init(SD_DISK_NAME);
        if (ret == 0) {
            break;
        }
        LOG_DBG("SD init attempt %d failed: %d", attempt + 1, ret);
    }
    if (ret < 0) {
        LOG_DBG("SD disk \"%s\" not found after retries: %d", SD_DISK_NAME, ret);
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
        /* Fall through — still need to deinit the disk controller */
    }

    g_mounted = false;
    /* Fully release the SDMMC host controller so the next akira_sd_card_init()
     * can reinitialize cleanly without ENOTSUP/-116 errors. */
    disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_CTRL_DEINIT, NULL);
    LOG_INF("SD card unmounted");
}

#ifdef CONFIG_AKIRA_SD_HOTPLUG
void akira_sd_card_deinit_force(void)
{
    if (!g_mounted) {
        return;
    }

    /* Card is physically gone — zero the FATFS object so fs_unmount skips
     * all disk I/O and returns instantly instead of timing out on the bus. */
    memset(&g_fat_fs, 0, sizeof(g_fat_fs));
    (void)fs_unmount(&g_sd_mount);
    g_mounted = false;
    LOG_INF("SD card force-unmounted");
}
#endif

static int sd_card_sys_init(void)
{
    int ret = akira_sd_card_init();

#ifdef CONFIG_AKIRA_SD_HOTPLUG
    hotplug_init();
#endif

    return ret;
}

/* Init before fs_manager (APPLICATION, priority 40) so sd_available is set */
SYS_INIT(sd_card_sys_init, APPLICATION, CONFIG_AKIRA_SD_INIT_PRIORITY);

#endif /* CONFIG_AKIRA_SD_CARD */
