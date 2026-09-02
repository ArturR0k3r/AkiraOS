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
 * Numeric button IDs passed to input_button_pressed() and returned as
 * bit positions in the bitmask from input_read_buttons().
 *
 * Bit N of the bitmask is set when button N is pressed:
 * @code
 * int mask = input_read_buttons();
 * if (mask & (1 << AKIRA_BTN_A)) { ... }
 * @endcode
 * @{
 */
/* These are the zephyr,code values emitted by the board's gpio-keys node —
 * keep them identical to the DTS, to src/api/akira_input_api.h and to the
 * SDK's akira_console.h (AKIRA_BTN_ID_*).  B, X and Y are NOT in alphabetical
 * order: they follow the silkscreen (SW1=Y, SW3=B, SW4=X). */
#define AKIRA_BTN_POWER 0    /**< System power / ON-OFF button */
#define AKIRA_BTN_SETTINGS 1 /**< System settings / Home / OK button */
#define AKIRA_BTN_UP 2       /**< D-pad Up */
#define AKIRA_BTN_DOWN 3     /**< D-pad Down */
#define AKIRA_BTN_LEFT 4     /**< D-pad Left */
#define AKIRA_BTN_RIGHT 5    /**< D-pad Right */
#define AKIRA_BTN_A 6        /**< Face button A (SW2, GPIO15) */
#define AKIRA_BTN_Y 7        /**< Face button Y (SW1, GPIO41) */
#define AKIRA_BTN_B 8        /**< Face button B (SW3, GPIO16) */
#define AKIRA_BTN_X 9        /**< Face button X (SW4, GPIO17) */
/** @} */

/** Number of buttons on the AkiraConsole. */
#define AKIRA_BTN_COUNT 10

/**
 * @brief Test whether a button is set in a bitmask returned by input_read_buttons().
 *
 * @param mask  Bitmask from input_read_buttons()
 * @param btn   Button ID (AKIRA_BTN_*)
 * @return Non-zero if button is pressed, 0 otherwise.
 */
#define AKIRA_BTN_IS_PRESSED(mask, btn) (((mask) >> (btn)) & 1U)

#endif /* AKIRACONSOLE_NATIVE_API_H */
