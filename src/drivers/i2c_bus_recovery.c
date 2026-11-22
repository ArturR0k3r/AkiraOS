/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file i2c_bus_recovery.c
 * @brief Free an I2C bus that a target left stuck across an MCU reset.
 *
 * I2C targets on an always-on rail are not reset with the MCU.  If the ESP32
 * resets mid-transfer (RESET button, watchdog, sys_reboot(), panic) while a
 * target is driving SDA low (an ACK bit, or a 0 bit of a byte it is returning),
 * that target keeps SDA low indefinitely: it is still waiting for the SCL
 * edges that would finish its byte.  Every transfer after the reboot fails.
 *
 * Zephyr's ESP32 I2C driver does not clear the bus at init, and on the
 * ESP32-S3 its runtime recovery cannot help either: i2c_hw_fsm_reset() requests
 * the hardware bus clear between clock_control_off() and clock_control_on(),
 * i.e. while the controller is held in reset, so the clear pulses are never
 * sent.  On AkiraConsole Prod a stuck bus takes out the TCA6408 expander, then
 * SPI2 (two of its chip selects are TCA6408 pins) and the ST7789V on it, and
 * the LP5817 backlight: the console boots to a dark screen.
 *
 * This runs once per `akira,i2c-bus-clear` devicetree node, before the I2C
 * driver claims the pins, and performs the standard bus clear (NXP UM10204,
 * "Bus clear"): clock SCL until the target releases SDA, at most nine pulses,
 * then send a STOP.  The pins live in their own node because the ESP32-S3 I2C
 * driver rejects scl-gpios/sda-gpios on the controller at build time.
 */

#define DT_DRV_COMPAT akira_i2c_bus_clear

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(i2c_bus_recovery, CONFIG_AKIRA_LOG_LEVEL);

/* After the GPIO driver (PRE_KERNEL_1 on ESP32; i2c_bus_clear() checks it is
 * ready), before the I2C driver applies its pinctrl.  SYS_INIT wants a
 * literal, hence the assert. */
#define I2C_BUS_RECOVERY_INIT_PRIORITY 45
BUILD_ASSERT(I2C_BUS_RECOVERY_INIT_PRIORITY < CONFIG_I2C_INIT_PRIORITY,
             "I2C bus recovery must run before the I2C driver initialises");

/* 100 kHz: slow enough for every target, and only ever ~10 bits long. */
#define HALF_PERIOD_US 5
/* A target releases SDA by the next ACK slot at the latest. */
#define MAX_SCL_PULSES 9

static void line_set(const struct gpio_dt_spec *g, int level)
{
    (void)gpio_pin_set_raw(g->port, g->pin, level);
    k_busy_wait(HALF_PERIOD_US);
}

static int line_get(const struct gpio_dt_spec *g)
{
    return gpio_pin_get_raw(g->port, g->pin);
}

static void i2c_bus_clear(const char *name, const struct gpio_dt_spec *scl,
                          const struct gpio_dt_spec *sda)
{
    if (!gpio_is_ready_dt(scl) || !gpio_is_ready_dt(sda))
    {
        LOG_WRN("%s: bus-clear GPIOs not ready", name);
        return;
    }

    /* Open drain with the input enabled: 1 releases the line to the board
     * pull-ups, and the pin still reads the level a target drives. */
    const gpio_flags_t od = GPIO_OUTPUT_HIGH | GPIO_OPEN_DRAIN | GPIO_INPUT;

    if (gpio_pin_configure_dt(scl, od) != 0 || gpio_pin_configure_dt(sda, od) != 0)
    {
        LOG_WRN("%s: cannot drive SCL/SDA for bus clear", name);
        return;
    }
    k_busy_wait(HALF_PERIOD_US);

    int pulses = 0;

    while (line_get(sda) == 0 && pulses < MAX_SCL_PULSES)
    {
        line_set(scl, 0);
        line_set(scl, 1);
        pulses++;
    }

    /* STOP (SDA rises while SCL is high), sent even on an idle bus: it also
     * resets a target that was mid-byte with SDA released, so the first START
     * after boot is seen cleanly.  Targets ignore a STOP with no transfer. */
    line_set(scl, 0);
    line_set(sda, 0);
    line_set(scl, 1);
    line_set(sda, 1);

    const int scl_level = line_get(scl);
    const int sda_level = line_get(sda);

    /* Release both lines; the I2C driver's pinctrl takes the pins next. */
    (void)gpio_pin_configure_dt(scl, GPIO_INPUT);
    (void)gpio_pin_configure_dt(sda, GPIO_INPUT);

    if (scl_level != 1 || sda_level != 1)
    {
        LOG_ERR("%s: bus still stuck after clear (SCL=%d SDA=%d)", name, scl_level,
                sda_level);
    }
    else if (pulses > 0)
    {
        LOG_WRN("%s: a target held SDA low across reset; released after %d SCL pulse(s)",
                name, pulses);
    }
}

#define I2C_BUS_CLEAR_INST(inst)                                                    \
    {                                                                               \
        static const struct gpio_dt_spec scl = GPIO_DT_SPEC_INST_GET(inst, scl_gpios); \
        static const struct gpio_dt_spec sda = GPIO_DT_SPEC_INST_GET(inst, sda_gpios); \
        i2c_bus_clear(DT_NODE_FULL_NAME(DT_INST_PHANDLE(inst, i2c_bus)), &scl, &sda); \
    }

static int i2c_bus_recovery_init(void)
{
    DT_INST_FOREACH_STATUS_OKAY(I2C_BUS_CLEAR_INST)
    return 0;
}

SYS_INIT(i2c_bus_recovery_init, POST_KERNEL, I2C_BUS_RECOVERY_INIT_PRIORITY);
