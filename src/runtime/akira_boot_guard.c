/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_boot_guard
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_boot_guard, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_boot_guard.c
 * @brief Software boot counter with automatic rollback.
 *
 * NVS key layout (partition: "boot_counter"):
 *   key 1 — akira_boot_counter_t (8 bytes)
 */

#ifdef CONFIG_AKIRA_BOOT_GUARD

#include <akira_boot_guard.h>
#include <connectivity/ota/ota_manager.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <string.h>
#include <inttypes.h>

#define BOOT_GUARD_NVS_ID    1u
#define BOOT_GUARD_MAGIC     0xB007C0DEu
#define BOOT_GUARD_SECTORS   2u

#define BOOT_GUARD_FLASH_LABEL boot_counter
#define BOOT_GUARD_FLASH_ID    FIXED_PARTITION_ID(BOOT_GUARD_FLASH_LABEL)

#ifndef CONFIG_AKIRA_OTA_MAX_BOOT_FAILURES
#define CONFIG_AKIRA_OTA_MAX_BOOT_FAILURES 3
#endif

typedef struct {
    uint32_t magic;
    uint8_t  boot_count;
    uint8_t  confirmed;
    uint8_t  _pad[2];
} akira_boot_counter_t;

static struct nvs_fs   g_nvs;
static bool            g_ready;
static uint8_t         g_current_count;

/* ── NVS helpers ────────────────────────────────────────────────────────── */

static int nvs_open_partition(void)
{
    const struct flash_area *fa;
    int ret = flash_area_open(BOOT_GUARD_FLASH_ID, &fa);
    if (ret < 0) {
        LOG_WRN("Boot guard: flash partition not available (%d)", ret);
        return ret;
    }
    g_nvs.sector_size  = fa->fa_size / BOOT_GUARD_SECTORS;
    g_nvs.sector_count = BOOT_GUARD_SECTORS;
    g_nvs.offset       = fa->fa_off;
    flash_area_close(fa);
    return nvs_mount(&g_nvs);
}

/* ── public API ─────────────────────────────────────────────────────────── */

int akira_boot_guard_init(void)
{
    int ret = nvs_open_partition();
    if (ret < 0) {
        LOG_WRN("Boot guard disabled (NVS: %d)", ret);
        return ret;
    }
    g_ready = true;

    akira_boot_counter_t rec = {0};
    ssize_t n = nvs_read(&g_nvs, BOOT_GUARD_NVS_ID, &rec, sizeof(rec));
    if (n != (ssize_t)sizeof(rec) || rec.magic != BOOT_GUARD_MAGIC) {
        /* First boot or corrupt record */
        rec.magic      = BOOT_GUARD_MAGIC;
        rec.boot_count = 1;
        rec.confirmed  = 0;
        LOG_INF("Boot guard: first boot, counter=1");
    } else if (rec.confirmed) {
        /* Previous boot was confirmed — reset counter */
        rec.confirmed  = 0;
        rec.boot_count = 1;
        LOG_INF("Boot guard: confirmed boot, reset counter");
    } else {
        rec.boot_count++;
        LOG_WRN("Boot guard: unconfirmed boot #%" PRIu8
                " (max %" PRIu8 ")",
                rec.boot_count,
                (uint8_t)CONFIG_AKIRA_OTA_MAX_BOOT_FAILURES);
    }

    g_current_count = rec.boot_count;
    (void)nvs_write(&g_nvs, BOOT_GUARD_NVS_ID, &rec, sizeof(rec));

    if (rec.boot_count >= (uint8_t)CONFIG_AKIRA_OTA_MAX_BOOT_FAILURES) {
        LOG_ERR("Boot guard: max failures reached — requesting rollback");
        (void)ota_request_rollback();
        /* Device will reboot inside ota_request_rollback().  If it returns,
         * tell the caller rollback was triggered. */
        return -EAGAIN;
    }
    return 0;
}

int akira_boot_guard_confirm(void)
{
    if (!g_ready) {
        return -ENODEV;
    }

    akira_boot_counter_t rec = {
        .magic      = BOOT_GUARD_MAGIC,
        .boot_count = 0,
        .confirmed  = 1,
    };
    g_current_count = 0;

    int ret = nvs_write(&g_nvs, BOOT_GUARD_NVS_ID, &rec, sizeof(rec));
    if (ret < 0) {
        LOG_ERR("Boot guard: NVS write failed (%d)", ret);
        return ret;
    }

    /* Also confirm at MCUboot level */
    enum ota_result ota_ret = ota_confirm_firmware();
    if (ota_ret != OTA_OK) {
        LOG_WRN("Boot guard: ota_confirm_firmware: %d", ota_ret);
    }

    LOG_INF("Boot guard: firmware confirmed");
    return 0;
}

uint8_t akira_boot_guard_count(void)
{
    return g_current_count;
}

#endif /* CONFIG_AKIRA_BOOT_GUARD */
