/**
 * @file akiraconsole_native_api.h
 * @brief AkiraConsole-specific WASM API extensions
 *
 * Button ID constants and input helpers for the AkiraConsole hardware:
 *   - Power and Settings system buttons
 *   - D-pad (4-way directional)
 *   - ABXY action face buttons
 *
 * Include this header **in addition to** akira_native_api.h only when
 * targeting AkiraConsole hardware or a compatible layout.
 * Generic AkiraOS targets (ESP32 DevKit, nRF54L15DK, STM32 Nucleo, …)
 * do not have this button layout — do not include this header in
 * board-agnostic application code.
 *
 * @stability experimental
 * @since 1.5
 */

#ifndef AKIRACONSOLE_NATIVE_API_H
#define AKIRACONSOLE_NATIVE_API_H

/**
 * @defgroup akiraconsole_btns AkiraConsole Button IDs
 *
 * Numeric button IDs for the AkiraConsole. These match the `zephyr,code`
 * values in the board's gpio-keys devicetree node.
 *
 * There is no button input native API — the runtime registers no input_*
 * symbol. These constants are kept for the LVGL input path
 * (CONFIG_AKIRA_LVGL_INPUT_ZEPHYR) and for native code reading the input
 * subsystem directly.
 * @{
 */
#define AKIRA_BTN_POWER    0 /**< System power / ON-OFF button */
#define AKIRA_BTN_SETTINGS 1 /**< System settings button */
#define AKIRA_BTN_UP       2 /**< D-pad Up */
#define AKIRA_BTN_DOWN     3 /**< D-pad Down */
#define AKIRA_BTN_LEFT     4 /**< D-pad Left */
#define AKIRA_BTN_RIGHT    5 /**< D-pad Right */
#define AKIRA_BTN_A        6 /**< Face button A */
#define AKIRA_BTN_B        7 /**< Face button B */
#define AKIRA_BTN_X        8 /**< Face button X */
#define AKIRA_BTN_Y        9 /**< Face button Y */
/** @} */

/** Number of buttons on the AkiraConsole. */
#define AKIRA_BTN_COUNT 10

/**
 * @brief Test whether a button is set in a button bitmask.
 *
 * @param mask  Bitmask where bit N is set when button N is held
 * @param btn   Button ID (AKIRA_BTN_*)
 * @return Non-zero if button is pressed, 0 otherwise.
 */
#define AKIRA_BTN_IS_PRESSED(mask, btn) (((mask) >> (btn)) & 1U)

#endif /* AKIRACONSOLE_NATIVE_API_H */
