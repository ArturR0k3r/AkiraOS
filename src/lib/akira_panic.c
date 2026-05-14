/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME akira_panic
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_panic, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_panic.c
 * @brief Panic handler — hooks k_sys_fatal_error_handler() to capture crash
 *        info into NVS so it can be reported on the next boot.
 *
 * Requires CONFIG_AKIRA_PANIC=y and CONFIG_NVS=y.
 * The NVS partition used is CONFIG_AKIRA_PANIC_PARTITION (default "panic_store").
 *
 * NVS layout:
 *   key 1 — akira_crash_record_t  (sizeof <= 64 bytes)
 */

#ifdef CONFIG_AKIRA_PANIC

#include <lib/akira_panic.h>
#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log_ctrl.h>
#include <string.h>

#define PANIC_NVS_ID      1u
#define CRASH_MAGIC       0xDEADC0DEu
#define PANIC_SECTOR_CNT  2u

/* NVS partition must be defined in DTS / flash_map as AKIRA_PANIC_PARTITION */
#define PANIC_FLASH_AREA_LABEL panic_store
#define PANIC_FLASH_AREA_ID    FIXED_PARTITION_ID(PANIC_FLASH_AREA_LABEL)

static struct nvs_fs g_panic_nvs;
static bool         g_nvs_ready;
static char         g_active_app[AKIRA_PANIC_APP_NAME_MAX];
static K_MUTEX_DEFINE(g_app_name_mutex);

/* ── public API ─────────────────────────────────────────────────────────── */

int akira_panic_init(void)
{
    const struct flash_area *fa;
    int ret = flash_area_open(PANIC_FLASH_AREA_ID, &fa);
    if (ret < 0) {
        LOG_WRN("Panic: flash area not available (%d) — no crash recording", ret);
        return ret;
    }

    g_panic_nvs.sector_size  = fa->fa_size / PANIC_SECTOR_CNT;
    g_panic_nvs.sector_count = PANIC_SECTOR_CNT;
    g_panic_nvs.offset       = fa->fa_off;
    flash_area_close(fa);

    ret = nvs_mount(&g_panic_nvs);
    if (ret < 0) {
        LOG_ERR("Panic: NVS mount failed (%d)", ret);
        return ret;
    }
    g_nvs_ready = true;

    /* Report any previous crash */
    akira_crash_record_t rec;
    if (akira_panic_has_record(&rec)) {
        LOG_ERR("=== PREVIOUS BOOT CRASH REPORT ===");
        LOG_ERR("  uptime_ms  : %" PRIu64,  rec.uptime_ms);
        LOG_ERR("  reason     : 0x%08" PRIx32, rec.reason);
        LOG_ERR("  fault_addr : 0x%08" PRIxPTR, rec.fault_addr);
        LOG_ERR("  stack_ptr  : 0x%08" PRIxPTR, rec.stack_ptr);
        LOG_ERR("  last app   : %s", rec.last_app[0] ? rec.last_app : "<none>");
        LOG_ERR("===================================");

        /* Erase the record */
        (void)nvs_delete(&g_panic_nvs, PANIC_NVS_ID);
    }
    return 0;
}

bool akira_panic_has_record(akira_crash_record_t *out)
{
    if (!g_nvs_ready || !out) {
        return false;
    }
    ssize_t n = nvs_read(&g_panic_nvs, PANIC_NVS_ID, out, sizeof(*out));
    if (n != (ssize_t)sizeof(*out)) {
        return false;
    }
    return (out->magic == CRASH_MAGIC);
}

void akira_panic_set_active_app(const char *name)
{
    k_mutex_lock(&g_app_name_mutex, K_FOREVER);
    if (name) {
        strncpy(g_active_app, name, sizeof(g_active_app) - 1);
        g_active_app[sizeof(g_active_app) - 1] = '\0';
    } else {
        g_active_app[0] = '\0';
    }
    k_mutex_unlock(&g_app_name_mutex);
}

/* ── fatal error hook ───────────────────────────────────────────────────── */

/**
 * Override the weak k_sys_fatal_error_handler provided by Zephyr.
 * This is intentionally NOT re-entrant — a double-fault will just hard-lock.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    /* Flush logs before we potentially corrupt everything */
    LOG_PANIC();

    if (g_nvs_ready) {
        akira_crash_record_t rec = {0};
        rec.magic      = CRASH_MAGIC;
        rec.uptime_ms  = (uint64_t)k_uptime_get();
        rec.reason     = (uint32_t)reason;
#if defined(CONFIG_CPU_CORTEX_M) && defined(CONFIG_EXCEPTION_DEBUG)
        if (esf) {
            rec.fault_addr = (uintptr_t)esf->basic.pc;
            rec.stack_ptr  = (uintptr_t)esf;
        }
#else
        (void)esf;
#endif
        k_mutex_lock(&g_app_name_mutex, K_NO_WAIT);
        strncpy(rec.last_app, g_active_app, sizeof(rec.last_app) - 1);
        k_mutex_unlock(&g_app_name_mutex);

        (void)nvs_write(&g_panic_nvs, PANIC_NVS_ID, &rec, sizeof(rec));
    }

    LOG_ERR("FATAL error %u — rebooting", reason);
    sys_reboot(SYS_REBOOT_COLD);
}

#endif /* CONFIG_AKIRA_PANIC */
