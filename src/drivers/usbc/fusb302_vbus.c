/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_fusb302_vbus
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_fusb302_vbus, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file fusb302_vbus.c
 * @brief VBUS-presence polling via the FUSB302 USB-C PD controller (U8).
 *
 * The ESP32-S3 dwc2 UDC driver never sets caps.can_detect_vbus (see
 * usb_manager_report_vbus_state()'s doc comment), so the OTG peripheral
 * itself can't tell us when a cable is plugged in. The FUSB302 already has
 * its own VBUS comparator for CC/PD attach detection (STATUS0.VBUSOK,
 * datasheet Table 37) — this polls that register directly over I2C instead
 * of wiring the chip's INT_N line, whose routing (TCA6408 P6) conflicts
 * with this board's own pin-map comment (P6 = LR_CS elsewhere in the same
 * DTS) and hasn't been confirmed against the schematic.
 *
 * Register values are verified against the FUSB302B datasheet (Tables 27,
 * 28, 37): POWER (0x0B) PWR[3:0], RESET (0x0C) bit0=SW_RES, STATUS0 (0x40)
 * bit7=VBUSOK.
 */

#include "fusb302_vbus.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>

#include <connectivity/usb/usb_manager.h>

#define FUSB302_REG_POWER 0x0B
#define FUSB302_REG_RESET 0x0C
#define FUSB302_REG_STATUS0 0x40

#define FUSB302_RESET_SW_RES BIT(0)
#define FUSB302_POWER_ALL BIT(3) | BIT(2) | BIT(1) | BIT(0) /* osc+measure+rx+bandgap */
#define FUSB302_STATUS0_VBUSOK BIT(7)

#define FUSB302_POLL_INTERVAL_MS 250

/* First poll deferred well past boot: firing this early (~1.1s) races the
 * display stack's own bring-up (backlight/panel "ready" logs land ~1.16-
 * 1.18s on this board) — a confirm dialog drawn before the panel is fully
 * ready renders as a black screen instead of the dialog. */
#define FUSB302_FIRST_POLL_DELAY_MS 5000

static const struct i2c_dt_spec fusb302 = I2C_DT_SPEC_GET(DT_NODELABEL(fusb302));

static bool g_vbus_present;
static bool g_chip_ready;

bool akira_fusb302_vbus_present(void)
{
    return g_chip_ready && g_vbus_present;
}

static void poll_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(poll_work, poll_work_fn);

static void poll_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    uint8_t status0;
    int ret = i2c_reg_read_byte_dt(&fusb302, FUSB302_REG_STATUS0, &status0);

    if (ret != 0) {
        LOG_WRN("STATUS0 read failed: %d", ret);
        goto reschedule;
    }

    bool present = (status0 & FUSB302_STATUS0_VBUSOK) != 0;

    if (present != g_vbus_present) {
        g_vbus_present = present;
        usb_manager_report_vbus_state(present);
    }

reschedule:
    k_work_reschedule(k_work_delayable_from_work(work),
                      K_MSEC(FUSB302_POLL_INTERVAL_MS));
}

static int fusb302_vbus_init(void)
{
    if (!i2c_is_ready_dt(&fusb302)) {
        LOG_ERR("FUSB302 I2C bus not ready");
        return -ENODEV;
    }

    int ret = i2c_reg_write_byte_dt(&fusb302, FUSB302_REG_RESET, FUSB302_RESET_SW_RES);
    if (ret != 0) {
        LOG_ERR("FUSB302 reset failed: %d", ret);
        return ret;
    }

    /* Let the reset settle before touching POWER — datasheet gives no
     * explicit delay, this mirrors typical reference-driver bring-up. */
    k_sleep(K_MSEC(10));

    ret = i2c_reg_write_byte_dt(&fusb302, FUSB302_REG_POWER, FUSB302_POWER_ALL);
    if (ret != 0) {
        LOG_ERR("FUSB302 power-up failed: %d", ret);
        return ret;
    }

    g_chip_ready = true;
    k_work_schedule(&poll_work, K_MSEC(FUSB302_FIRST_POLL_DELAY_MS));
    LOG_INF("FUSB302 VBUS sense ready (first poll in %d ms, then every %d ms)",
            FUSB302_FIRST_POLL_DELAY_MS, FUSB302_POLL_INTERVAL_MS);
    return 0;
}

SYS_INIT(fusb302_vbus_init, APPLICATION, CONFIG_AKIRA_FUSB302_VBUS_INIT_PRIORITY);
