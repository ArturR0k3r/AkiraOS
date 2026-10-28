/*
 * Copyright (c) 2026 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_fusb302_vbus
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_fusb302_vbus, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file fusb302_vbus.c
 * @brief VBUS-presence sensing via the FUSB302 USB-C PD controller (U8).
 *
 * The ESP32-S3 dwc2 UDC driver never sets caps.can_detect_vbus (see
 * usb_manager_report_vbus_state()'s doc comment), so the OTG peripheral
 * itself can't tell us when a cable is plugged in. The FUSB302 already has
 * its own VBUS comparator for CC/PD attach detection (STATUS0.VBUSOK,
 * datasheet Table 37) — this reads that register over I2C.
 *
 * Wake-up wiring: INT_N goes to ESP32 GPIO3 (net /PWR/BAT_INT), an open-drain
 * line SHARED with the BQ25601 ~INT and the STC3115 ALM (the latter now
 * disabled by the gauge driver). A status change pulses the line low for
 * ~256 µs. We take a falling-edge interrupt on that pin so a plug-in is acted
 * on immediately, and keep a slow (1 s) poll as a safety net for the VBUSOK
 * edge the FUSB302 may latch without re-asserting INT_N. (The earlier revision
 * of this file claimed INT_N landed on TCA6408 P6 — that was wrong; P6 is not
 * on this net at all.)
 *
 * Because the line is shared, every falling edge triggers both a VBUS re-check
 * and a battery re-poll (the charger ~INT uses the same wire), so a charger
 * status change is serviced on the same event.
 *
 * Power: we keep only the POWER bits VBUSOK needs (bandgap + measure block)
 * and mask every FUSB302 interrupt source so the chip never holds the shared
 * INT_N line low against the other devices on it.
 *
 * Register values are verified against the FUSB302B datasheet (Tables 25-28,
 * 37): POWER (0x0B) PWR[3:0], RESET (0x0C) bit0=SW_RES, MASK1 (0x0A),
 * MASKA (0x0E), MASKB (0x0F), INTERRUPTA/B (0x3E/0x3F) and INTERRUPT (0x42)
 * are read-to-clear, STATUS0 (0x40) bit7=VBUSOK.
 */

#include "fusb302_vbus.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>

#include <connectivity/usb/usb_manager.h>

#if defined(CONFIG_AKIRA_POWER_MANAGER)
#include <drivers/power/power_manager.h>
#endif

#define FUSB302_REG_POWER      0x0B
#define FUSB302_REG_RESET      0x0C
#define FUSB302_REG_MASK1      0x0A
#define FUSB302_REG_MASKA      0x0E
#define FUSB302_REG_MASKB      0x0F
#define FUSB302_REG_INTERRUPTA 0x3E
#define FUSB302_REG_INTERRUPTB 0x3F
#define FUSB302_REG_STATUS0    0x40
#define FUSB302_REG_INTERRUPT  0x42

#define FUSB302_RESET_SW_RES   BIT(0)

/* POWER bits (datasheet Table 27): [0]=bandgap/wake, [1]=receiver+current
 * refs, [2]=measure block, [3]=internal oscillator.  The STATUS0.VBUSOK
 * comparator is fed from the measure block referenced to the bandgap, so
 * bandgap+measure is the minimum that keeps VBUSOK valid — the oscillator and
 * the PD receiver chain are only needed for actual PD messaging, which this
 * driver does not do.  (If VBUSOK ever reads stuck on real hardware, the next
 * thing to try is adding PWR[1] → 0x07.) */
#define FUSB302_POWER_VBUSOK   (BIT(0) | BIT(2)) /* 0x05 */

/* Mask-everything values: 1 = source masked.  MASK1/MASKA have 8 maskable
 * sources each; MASKB has only bit0 (M_GCRCSENT). */
#define FUSB302_MASK1_ALL      0xFF
#define FUSB302_MASKA_ALL      0xFF
#define FUSB302_MASKB_ALL      0x01

#define FUSB302_STATUS0_VBUSOK BIT(7)

/* First poll deferred well past boot: firing this early (~1.1s) races the
 * display stack's own bring-up (backlight/panel "ready" logs land ~1.16-
 * 1.18s on this board) — a confirm dialog drawn before the panel is fully
 * ready renders as a black screen instead of the dialog. */
#define FUSB302_FIRST_POLL_DELAY_MS 5000

static const struct i2c_dt_spec fusb302 = I2C_DT_SPEC_GET(DT_NODELABEL(fusb302));

/* Shared /PWR/BAT_INT line — declared on the bq25601 node (int-gpios =
 * <&gpio0 3 GPIO_ACTIVE_LOW>); the charger driver only configures it as a
 * plain input, so wiring the falling-edge interrupt here does not clash. */
static const struct gpio_dt_spec bat_int =
    GPIO_DT_SPEC_GET_OR(DT_NODELABEL(bq25601), int_gpios, {0});
static struct gpio_callback bat_int_cb_data;

static bool g_vbus_present;
static bool g_chip_ready;
static bool g_poll_idle;   /* true → periodic poll paused (blanked) */

bool akira_fusb302_vbus_present(void)
{
    return g_chip_ready && g_vbus_present;
}

/* Read VBUSOK and push a VBUS attach/detach edge to the USB manager on change.
 * Shared by the periodic poll and the INT_N work item. */
static void fusb302_vbus_check(void)
{
    uint8_t status0;
    int ret = i2c_reg_read_byte_dt(&fusb302, FUSB302_REG_STATUS0, &status0);

    if (ret != 0) {
        LOG_WRN("STATUS0 read failed: %d", ret);
        return;
    }

    bool present = (status0 & FUSB302_STATUS0_VBUSOK) != 0;

    if (present != g_vbus_present) {
        g_vbus_present = present;
#if defined(CONFIG_AKIRA_USB)
        usb_manager_report_vbus_state(present);
#endif
    }
}

static void poll_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(poll_work, poll_work_fn);

static void poll_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    fusb302_vbus_check();

    /* Stay quiet while blanked — the GPIO3 interrupt still services real
     * plug-in/charger edges, which is the only thing that matters then. */
    if (!g_poll_idle) {
        k_work_reschedule(&poll_work, K_MSEC(CONFIG_AKIRA_FUSB302_POLL_MS));
    }
}

/* Runs off the INT_N falling edge.  Immediate VBUS re-check plus a battery
 * re-poll, since the charger ~INT shares this line. */
static void int_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    fusb302_vbus_check();
#if defined(CONFIG_AKIRA_POWER_MANAGER)
    /* Declared in power_manager.h as safe to call from a work item. */
    akira_pm_battery_poll_now();
#endif
}
static K_WORK_DEFINE(int_work, int_work_fn);

static void bat_int_isr(const struct device *port,
                        struct gpio_callback *cb, gpio_port_pins_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);
    /* ISR context: hand everything off to the system workqueue. */
    k_work_submit(&int_work);
}

void akira_fusb302_set_idle(bool idle)
{
    g_poll_idle = idle;

    if (idle) {
        /* Pause the periodic poll; the in-flight handler sees g_poll_idle and
         * will not re-arm itself. */
        k_work_cancel_delayable(&poll_work);
    } else if (g_chip_ready) {
        /* Resume: check straight away, then the handler re-arms the cadence. */
        k_work_reschedule(&poll_work, K_NO_WAIT);
    }
}

void akira_fusb302_power_down(void)
{
    /* Called right before ESP32 deep sleep.  Stop polling and drop the chip to
     * POWER=0x00 — the SoC reboots on wake and fusb302_vbus_init() runs again,
     * so there is no state to restore. */
    g_poll_idle = true;
    k_work_cancel_delayable(&poll_work);

    if (g_chip_ready) {
        (void)i2c_reg_write_byte_dt(&fusb302, FUSB302_REG_POWER, 0x00);
    }
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

    /* Mask every interrupt source so the chip never pulls the shared INT_N
     * line low, then read the INTERRUPT* registers (read-to-clear) to release
     * any edge latched during/after reset. */
    (void)i2c_reg_write_byte_dt(&fusb302, FUSB302_REG_MASK1, FUSB302_MASK1_ALL);
    (void)i2c_reg_write_byte_dt(&fusb302, FUSB302_REG_MASKA, FUSB302_MASKA_ALL);
    (void)i2c_reg_write_byte_dt(&fusb302, FUSB302_REG_MASKB, FUSB302_MASKB_ALL);

    uint8_t scratch;
    (void)i2c_reg_read_byte_dt(&fusb302, FUSB302_REG_INTERRUPTA, &scratch);
    (void)i2c_reg_read_byte_dt(&fusb302, FUSB302_REG_INTERRUPTB, &scratch);
    (void)i2c_reg_read_byte_dt(&fusb302, FUSB302_REG_INTERRUPT, &scratch);

    /* Only the blocks VBUSOK needs — see FUSB302_POWER_VBUSOK. */
    ret = i2c_reg_write_byte_dt(&fusb302, FUSB302_REG_POWER, FUSB302_POWER_VBUSOK);
    if (ret != 0) {
        LOG_ERR("FUSB302 power-up failed: %d", ret);
        return ret;
    }

    g_chip_ready = true;

    /* Falling-edge wake on the shared /PWR/BAT_INT line (GPIO3).  Best-effort:
     * if the pin can't be set up we still have the poll. */
    if (!gpio_is_ready_dt(&bat_int)) {
        LOG_WRN("PWR_INT GPIO not ready; VBUS sense on poll only");
    } else {
        int gret = gpio_pin_configure_dt(&bat_int, GPIO_INPUT);
        if (gret == 0) {
            gret = gpio_pin_interrupt_configure_dt(&bat_int, GPIO_INT_EDGE_TO_ACTIVE);
        }
        if (gret == 0) {
            gpio_init_callback(&bat_int_cb_data, bat_int_isr, BIT(bat_int.pin));
            gret = gpio_add_callback(bat_int.port, &bat_int_cb_data);
        }
        if (gret != 0) {
            LOG_WRN("PWR_INT (GPIO3) irq setup failed: %d; poll only", gret);
        } else {
            LOG_INF("PWR_INT (GPIO3) falling-edge wired to VBUS/battery check");
        }
    }

    k_work_schedule(&poll_work, K_MSEC(FUSB302_FIRST_POLL_DELAY_MS));
    LOG_INF("FUSB302 VBUS sense ready (first poll in %d ms, then every %d ms)",
            FUSB302_FIRST_POLL_DELAY_MS, CONFIG_AKIRA_FUSB302_POLL_MS);
    return 0;
}

SYS_INIT(fusb302_vbus_init, APPLICATION, CONFIG_AKIRA_FUSB302_VBUS_INIT_PRIORITY);
