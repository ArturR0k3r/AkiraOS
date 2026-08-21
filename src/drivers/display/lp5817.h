/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: GPL-3.0-only
 */

/**
 * @file lp5817.h
 * @brief TI LP5817 3-channel I2C LED driver — AkiraConsole display backlight.
 *
 * On AkiraConsole Production (Rev A.3) the ST7789V panel's backlight LED
 * strings are NOT driven by a PWM GPIO.  U7 (LP5817DRLR) sinks the three
 * strings on J7 pins 5/6/7 and is controlled entirely over I2C0:
 *
 *   LP5817 pin 1 SCL  -> I2C0 SCL (GPIO18)
 *   LP5817 pin 2 SDA  -> I2C0 SDA (GPIO8)
 *   LP5817 pin 4 VCC  -> +3.3V
 *   LP5817 pin 8 OUT0 -> J7 pin 5   (LED string 0 cathode)
 *   LP5817 pin 7 OUT1 -> J7 pin 7   (LED string 1 cathode)
 *   LP5817 pin 6 OUT2 -> J7 pin 6   (LED string 2 cathode)
 *
 * The device powers up with every register at 0x00 (CHIP_EN clear), so the
 * backlight stays dark until software enables it — which is why the panel
 * reads as completely dead before this driver runs.
 */

#ifndef AKIRA_LP5817_H
#define AKIRA_LP5817_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the LP5817 and enable all three backlight channels.
 *
 * Performs a soft reset, enables the chip, turns on OUT0..OUT2, programs the
 * per-channel analog dot-current to full scale, and latches the configuration
 * with the update command.  Brightness is left at 0; call
 * akira_lp5817_set_brightness() to light the panel.
 *
 * @return 0 on success, negative errno on error.
 */
int akira_lp5817_init(void);

/**
 * @brief Set backlight brightness.
 *
 * Writes the 8-bit manual PWM value to all three channels and latches it.
 *
 * @param brightness 0 = off, 255 = full scale.
 * @return 0 on success, negative errno on error.
 */
int akira_lp5817_set_brightness(uint8_t brightness);

/**
 * @brief Whether the LP5817 was found and initialised successfully.
 */
bool akira_lp5817_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_LP5817_H */
