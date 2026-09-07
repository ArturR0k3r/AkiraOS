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
 * Hardware-agnostic: driver selected via board .conf/.overlay.
 * Runs at APPLICATION level, CONFIG_AKIRA_SD_INIT_PRIORITY (default 38).
 *
 * Hotplug (CONFIG_AKIRA_SD_HOTPLUG): polls TCA6408 P7 (SD_DET) via I2C
 * every POLL_MS. Heavy work runs on dedicated sd_event work queue.
 * Force-unmount zeroes FATFS object to skip SPI on card removal.
 * Re-insertion waits 300ms before CTRL_DEINIT to let card stabilize.
 */

#ifdef CONFIG_AKIRA_SD_CARD

#include "sd_card.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/drivers/disk.h>
#include <ff.h>
#include <errno.h>
#include <drivers/power/power_manager.h>
#if defined(CONFIG_AKIRA_SD_XIP) && defined(CONFIG_AKIRA_APP_MANAGER)
#include <runtime/app_manager/app_manager.h>
#endif
#include "lib/mem_helper.h"

/* Disk name must match `disk-name` in the DTS mmc{} node */
#define SD_DISK_NAME   "SD"
#define SD_MOUNT_POINT "/SD:"
#define SD_APPS_DIR    "/SD:/apps"

/* Generous bound on a companion BLE transfer; a missed teardown self-heals
 * instead of pinning the device awake forever. */
#define SD_TRANSFER_INSOMNIA_MAX_MS (5 * 60 * 1000)

static bool  g_mounted;
static volatile bool g_transfer_active;
static int   g_transfer_insomnia_handle = -1; /* -1 = no lock held */
static FATFS g_fat_fs AKIRA_BULK_BSS;

static struct fs_mount_t g_sd_mount = {
    .type      = FS_FATFS,
    .fs_data   = &g_fat_fs,
    .mnt_point = SD_MOUNT_POINT,
};

/* ------------------------------------------------------------------ */
/* Hotplug                                                              */
/* ------------------------------------------------------------------ */
#ifdef CONFIG_AKIRA_SD_HOTPLUG

#include <zephyr/drivers/gpio.h>

#if defined(CONFIG_AKIRA_USB_MSC)
#include "usb_msc.h"
#define AKIRA_MSC_OWNS_SD() akira_usb_msc_owns_sd()
#else
#define AKIRA_MSC_OWNS_SD() (false)
#endif

#define SD_DET_NODE  DT_NODELABEL(tca6408)
#define SD_DET_PIN   7   /* P7 = SD_DET, active-low = card present */

typedef struct {
    akira_sd_hotplug_cb_t cb;
    void                 *user_data;
} hotplug_entry_t;

static hotplug_entry_t g_hotplug_cbs[CONFIG_AKIRA_SD_HOTPLUG_MAX_CBS];

/* Pre-insert callback — fires before akira_sd_card_init() so UI can show
 * a loading indicator immediately when card is detected. */
static akira_sd_hotplug_cb_t g_pre_insert_cb;
static void                 *g_pre_insert_user;
static const struct device  *g_tca_dev;
static int                   g_last_det  = -1;    /* -1 = unknown */
static bool                  g_event_pending;     /* guard against double-submit */

/* Dedicated work queue — fs_unmount + registry ops need ~3KB stack */
#define SD_EVENT_WQ_STACK  4192
#define SD_EVENT_WQ_PRIO   11

/* Stack in PSRAM: this queue only mounts/unmounts the SD card over SPI and
 * updates the in-RAM registry — the internal flash is never written from it. */
AKIRA_BULK_STACK_DEFINE(g_sd_event_stack, SD_EVENT_WQ_STACK);
static struct k_work_q  g_sd_event_wq;
static struct k_work    g_sd_event_work;
static bool             g_sd_event_present;

static void sd_event_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    bool present = g_sd_event_present;
    g_event_pending = false;

    if (present && !g_mounted && !AKIRA_MSC_OWNS_SD()) {
        LOG_INF("SD hotplug: card inserted");
        if (g_pre_insert_cb) {
            g_pre_insert_cb(true, g_pre_insert_user);
        }
        int ret = akira_sd_card_init();
        if (ret < 0) {
            LOG_ERR("SD hotplug mount failed: %d", ret);
        } else {
            for (int i = 0; i < CONFIG_AKIRA_SD_HOTPLUG_MAX_CBS; i++) {
                if (g_hotplug_cbs[i].cb) {
                    g_hotplug_cbs[i].cb(true, g_hotplug_cbs[i].user_data);
                }
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

/* Poll runs on sysworkq — only does I2C read, submits event on change */
static void poll_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_poll_work, poll_work_fn);

/* Set while the display is blanked.  A card cannot be inserted into a device
 * sitting in a pocket, so the 2 Hz I2C transaction to the TCA6408 is pure drain
 * in that state.  Paused polls do not reschedule; resume does one immediate
 * poll so any change that happened while asleep is caught at wake. */
static bool g_poll_paused;

static void poll_work_fn(struct k_work *work)
{
    if (g_poll_paused) {
        return;  /* do not reschedule — akira_sd_card_hotplug_resume() restarts us */
    }

    int val = gpio_pin_get(g_tca_dev, SD_DET_PIN);
    if (val < 0) {
        LOG_ERR("SD_DET read failed: %d", val);
        goto reschedule;
    }

    if (val != g_last_det && !g_event_pending) {
        g_last_det      = val;
        g_sd_event_present = (val == 1);
        g_event_pending = true;
        k_work_submit_to_queue(&g_sd_event_wq, &g_sd_event_work);
    }

reschedule:
    k_work_reschedule(k_work_delayable_from_work(work),
                      K_MSEC(CONFIG_AKIRA_SD_HOTPLUG_POLL_MS));
}

void akira_sd_card_hotplug_pause(void)
{
    g_poll_paused = true;
    k_work_cancel_delayable(&g_poll_work);
}

void akira_sd_card_hotplug_resume(void)
{
    if (!g_poll_paused) {
        return;
    }
    g_poll_paused = false;
    /* Immediate poll: the card may have been swapped while we were blanked. */
    k_work_reschedule(&g_poll_work, K_NO_WAIT);
}

/* True while we unmounted the card purely to save power, so wake knows to put
 * it back.  Distinct from a user eject, which clears g_mounted via the hotplug
 * path and leaves this false. */
static bool g_idle_unmounted;

bool akira_sd_card_idle_unmount(void)
{
    if (!g_mounted || g_idle_unmounted) {
        return false;
    }

    if (akira_sd_card_is_transfer_active()) {
        return false;
    }

#if defined(CONFIG_AKIRA_SD_XIP) && defined(CONFIG_AKIRA_APP_MANAGER)
    /* With XIP, a running app's module may still be backed by the card.
     * Unmounting under it would fault the app, so never unmount while anything
     * is running — the power saved is not worth killing the user's app. */
    if (app_manager_get_running_count() > 0) {
        return false;
    }
#endif

    akira_sd_card_deinit();
    g_idle_unmounted = true;
    return true;
}

void akira_sd_card_idle_remount(void)
{
    if (!g_idle_unmounted) {
        return;
    }
    g_idle_unmounted = false;

    /* Best effort: if the card was pulled while we were blanked the mount
     * fails, and the hotplug poll resuming alongside us reports the removal. */
    (void)akira_sd_card_init();
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
    g_tca_dev = DEVICE_DT_GET(SD_DET_NODE);
    if (!device_is_ready(g_tca_dev)) {
        LOG_ERR("TCA6408 not ready");
        return -ENODEV;
    }

    int ret = gpio_pin_configure(g_tca_dev, SD_DET_PIN,
                                 GPIO_INPUT | GPIO_ACTIVE_LOW);
    if (ret < 0) {
        LOG_ERR("SD_DET configure failed: %d", ret);
        return ret;
    }

    k_work_queue_start(&g_sd_event_wq, g_sd_event_stack,
                       K_THREAD_STACK_SIZEOF(g_sd_event_stack),
                       SD_EVENT_WQ_PRIO, NULL);
    k_thread_name_set(&g_sd_event_wq.thread, "sd_event");
    k_work_init(&g_sd_event_work, sd_event_work_fn);

    k_work_schedule(&g_poll_work, K_MSEC(CONFIG_AKIRA_SD_HOTPLUG_POLL_MS));
    LOG_INF("SD hotplug ready (TCA6408 P7, %d ms poll)",
            CONFIG_AKIRA_SD_HOTPLUG_POLL_MS);
    return 0;
}

#endif /* CONFIG_AKIRA_SD_HOTPLUG */

/* ------------------------------------------------------------------ */
/* Core mount / unmount                                                 */
/* ------------------------------------------------------------------ */

int akira_sd_card_init(void)
{
    if (g_mounted) {
        return 0;
    }

    disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_CTRL_DEINIT, NULL);

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
        LOG_DBG("SD disk not found: %d", ret);
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

void akira_sd_card_set_transfer_active(bool active)
{
    /* bool is the source of truth (never gated on insomnia_enter() possibly
     * failing with -ENOMEM); the lock is just a self-healing backstop. */
    g_transfer_active = active;

    if (active) {
        if (g_transfer_insomnia_handle < 0) {
            g_transfer_insomnia_handle = akira_pm_insomnia_enter(
                "sd-transfer", SD_TRANSFER_INSOMNIA_MAX_MS);
        }
    } else {
        if (g_transfer_insomnia_handle >= 0) {
            akira_pm_insomnia_exit(g_transfer_insomnia_handle);
            g_transfer_insomnia_handle = -1;
        }
    }
}

bool akira_sd_card_is_transfer_active(void)
{
    return g_transfer_active;
}

void akira_sd_card_deinit_force(void)
{
    if (!g_mounted) {
        return;
    }

    /* Card physically absent — zero FATFS object so fs_unmount skips
     * all disk I/O and returns instantly. No SPI, no display lag. */
    memset(&g_fat_fs, 0, sizeof(g_fat_fs));
    (void)fs_unmount(&g_sd_mount);
    g_mounted = false;
    LOG_INF("SD card force-unmounted");
}

void akira_sd_card_deinit(void)
{
    if (!g_mounted) {
        return;
    }

    int ret = fs_unmount(&g_sd_mount);
    if (ret < 0) {
        LOG_WRN("SD unmount failed: %d", ret);
    }

    g_mounted = false;
    disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_CTRL_DEINIT, NULL);
    LOG_INF("SD card unmounted");
}

void akira_sd_card_release_for_usb_msc(void)
{
    if (!g_mounted) {
        return;
    }

    int ret = fs_unmount(&g_sd_mount);
    if (ret < 0) {
        LOG_WRN("SD unmount (USB MSC) failed: %d", ret);
    }

    g_mounted = false;
    LOG_INF("SD card released to USB MSC");
}

/* ------------------------------------------------------------------ */
/* SYS_INIT                                                             */
/* ------------------------------------------------------------------ */

static int sd_card_sys_init(void)
{
    akira_sd_card_init();

#ifdef CONFIG_AKIRA_SD_HOTPLUG
    hotplug_init();
#endif

    return 0;
}

/* Init before fs_manager (APPLICATION, priority 40) so sd_available is set */
SYS_INIT(sd_card_sys_init, APPLICATION, CONFIG_AKIRA_SD_INIT_PRIORITY);

#endif /* CONFIG_AKIRA_SD_CARD */
