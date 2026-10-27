/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_boot_guard
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_boot_guard, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_boot_guard.c
 * @brief Confirm a new image after a healthy boot, reboot it otherwise.
 *
 * MCUboot does the actual rollback: a trial image that resets before it is
 * confirmed is swapped back out. This module only has to make sure a bad
 * image resets (deadline timer, fatal-error handler) and a good one gets
 * confirmed (after AKIRA_HOOK_BOOT_READY plus a stability window).
 *
 * Rollback detection: at the start of a trial boot the identity of the trial
 * image (version + size from its MCUboot header) is stored under the settings
 * key "ota/trial" and removed when the image is confirmed. A confirmed boot
 * that still finds the key running a different image is the old firmware
 * MCUboot restored after the trial failed.
 */

#ifdef CONFIG_AKIRA_BOOT_GUARD

#include <akira_boot_guard.h>
#include <akira_hooks.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/fatal.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#ifdef CONFIG_AKIRA_SETTINGS
#include "settings/settings.h"
#endif

#define TRIAL_RECORD_KEY "ota/trial"
/* "255.255.65535+4294967295/4294967295" */
#define IMAGE_ID_LEN     40

#if !defined(CONFIG_AKIRA_BOOT_GUARD_MANUAL_CONFIRM)
BUILD_ASSERT(CONFIG_AKIRA_BOOT_GUARD_CONFIRM_DELAY_S <
             CONFIG_AKIRA_BOOT_GUARD_TRIAL_TIMEOUT_S,
             "the trial deadline must leave time for the auto-confirm delay");
#endif

static atomic_t g_trial;
static bool g_rolled_back;

static void reboot_to_roll_back(const char *why)
{
    LOG_ERR("%s: rebooting, MCUboot will restore the previous firmware", why);
    LOG_PANIC();
    sys_reboot(SYS_REBOOT_COLD);
}

static void deadline_expired(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    reboot_to_roll_back("Trial firmware not confirmed in time");
}

static K_TIMER_DEFINE(g_deadline, deadline_expired, NULL);

static void confirm_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    (void)akira_boot_guard_confirm();
}

static K_WORK_DELAYABLE_DEFINE(g_confirm_work, confirm_work_fn);

/* ── trial record ───────────────────────────────────────────────────────── */

static void image_identity(char *buf, size_t len)
{
    struct mcuboot_img_header hdr;
    int rc = boot_read_bank_header(FIXED_PARTITION_ID(slot0_partition), &hdr,
                                   sizeof(hdr));
    if (rc != 0) {
        buf[0] = '\0';
        return;
    }
    snprintk(buf, len, "%u.%u.%u+%" PRIu32 "/%" PRIu32, hdr.h.v1.sem_ver.major,
             hdr.h.v1.sem_ver.minor, hdr.h.v1.sem_ver.revision,
             hdr.h.v1.sem_ver.build_num, hdr.h.v1.image_size);
}

static int record_load(char *buf, size_t len)
{
#ifdef CONFIG_AKIRA_SETTINGS
    return akira_settings_get(TRIAL_RECORD_KEY, buf, len);
#else
    ARG_UNUSED(buf);
    ARG_UNUSED(len);
    return -ENOTSUP;
#endif
}

static void record_store(const char *id)
{
#ifdef CONFIG_AKIRA_SETTINGS
    int rc = akira_settings_set(TRIAL_RECORD_KEY, id, 0);
    if (rc != 0) {
        LOG_WRN("Cannot record trial image (%d): a rollback will not be reported", rc);
    }
#else
    ARG_UNUSED(id);
#endif
}

/* The secondary slot now holds the image that failed. Wipe its header so it
 * can never be booted again, e.g. by a manual "roll back" to it. */
static void discard_failed_image(void)
{
#ifdef CONFIG_FLASH_PAGE_LAYOUT
    const struct flash_area *fa;
    struct flash_pages_info page;

    if (flash_area_open(FIXED_PARTITION_ID(slot1_partition), &fa) != 0) {
        return;
    }
    if (flash_get_page_info_by_offs(flash_area_get_device(fa), fa->fa_off, &page) == 0 &&
        flash_area_erase(fa, 0, page.size) == 0) {
        LOG_INF("Discarded the failed image");
    }
    flash_area_close(fa);
#endif
}

static void record_clear(void)
{
#ifdef CONFIG_AKIRA_SETTINGS
    (void)akira_settings_delete(TRIAL_RECORD_KEY);
#endif
}

/* ── public API ─────────────────────────────────────────────────────────── */

int akira_boot_guard_init(void)
{
    char id[IMAGE_ID_LEN];

    k_timer_stop(&g_deadline);
    (void)k_work_cancel_delayable(&g_confirm_work);
    atomic_clear(&g_trial);
    g_rolled_back = false;

    image_identity(id, sizeof(id));

    if (!boot_is_img_confirmed()) {
        atomic_set(&g_trial, 1);
        record_store(id);
        k_timer_start(&g_deadline,
                      K_SECONDS(CONFIG_AKIRA_BOOT_GUARD_TRIAL_TIMEOUT_S),
                      K_NO_WAIT);
        LOG_WRN("Trial boot of firmware %s: rolls back unless confirmed within %d s",
                id, CONFIG_AKIRA_BOOT_GUARD_TRIAL_TIMEOUT_S);
        return 0;
    }

    char trial[IMAGE_ID_LEN];
    if (record_load(trial, sizeof(trial)) == 0) {
        record_clear();
        if (strcmp(trial, id) != 0) {
            g_rolled_back = true;
            LOG_ERR("Firmware %s failed its trial boot; rolled back to %s", trial, id);
            discard_failed_image();
        }
    }
    return 0;
}

int akira_boot_guard_confirm(void)
{
    if (!atomic_get(&g_trial)) {
        return 0;
    }

    int rc = boot_write_img_confirmed();
    if (rc != 0) {
        LOG_ERR("Cannot confirm firmware (%d)", rc);
        return rc;
    }
    if (!atomic_cas(&g_trial, 1, 0)) {
        return 0; /* confirmed concurrently */
    }

    k_timer_stop(&g_deadline);
    (void)k_work_cancel_delayable(&g_confirm_work);
    record_clear();
    LOG_INF("Firmware confirmed");

    struct akira_hook_event evt = { .type = AKIRA_HOOK_OTA_CONFIRMED };
    akira_hooks_emit(&evt);
    return 0;
}

int akira_boot_guard_reject(void)
{
    if (!atomic_get(&g_trial)) {
        return -EALREADY;
    }
    reboot_to_roll_back("Trial firmware rejected");
    return 0;
}

bool akira_boot_guard_is_trial(void)
{
    return atomic_get(&g_trial) != 0;
}

bool akira_boot_guard_rolled_back(void)
{
    return g_rolled_back;
}

/* ── boot integration ───────────────────────────────────────────────────── */

static void on_boot_ready(const struct akira_hook_event *event, void *user_data)
{
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);

    if (g_rolled_back) {
        struct akira_hook_event evt = {
            .type = AKIRA_HOOK_OTA_ERROR,
            .ota = { .error = -ECANCELED },
        };
        akira_hooks_emit(&evt);
    }

#ifndef CONFIG_AKIRA_BOOT_GUARD_MANUAL_CONFIRM
    if (atomic_get(&g_trial)) {
        LOG_INF("Boot complete: confirming firmware in %d s",
                CONFIG_AKIRA_BOOT_GUARD_CONFIRM_DELAY_S);
        k_work_schedule(&g_confirm_work,
                        K_SECONDS(CONFIG_AKIRA_BOOT_GUARD_CONFIRM_DELAY_S));
    }
#endif
}

AKIRA_HOOK_DEFINE(akira_boot_guard_hook, AKIRA_HOOK_MASK(AKIRA_HOOK_BOOT_READY),
                  on_boot_ready, NULL);

static int boot_guard_sys_init(void)
{
    return akira_boot_guard_init();
}

SYS_INIT(boot_guard_sys_init, APPLICATION, CONFIG_AKIRA_BOOT_GUARD_INIT_PRIORITY);

#ifdef CONFIG_AKIRA_BOOT_GUARD_FATAL_REBOOT
/* Zephyr's default handler halts, which would leave a crashed trial image
 * running forever instead of letting MCUboot roll it back. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    ARG_UNUSED(esf);

    LOG_PANIC();
    if (atomic_get(&g_trial)) {
        LOG_ERR("Fatal error %u in trial firmware", reason);
        reboot_to_roll_back("Trial firmware crashed");
    }
    LOG_ERR("Halting system");
    k_fatal_halt(reason);
}
#endif

#endif /* CONFIG_AKIRA_BOOT_GUARD */
