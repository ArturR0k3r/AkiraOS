/**
 * @file akira_buttons.h
 * @brief Direct GPIO Button Driver API for Akira Board
 */

#ifndef AKIRA_BUTTONS_H
#define AKIRA_BUTTONS_H

#include <stdint.h>

int akira_buttons_init(void);
uint16_t akira_buttons_get_state(void);

// Button bitmask definitions (match enum gaming_button)
#define BTN_ONOFF (1 << 0)
#define BTN_SETTINGS (1 << 1)
#define BTN_UP (1 << 2)
#define BTN_DOWN (1 << 3)
#define BTN_LEFT (1 << 4)
#define BTN_RIGHT (1 << 5)
#define BTN_A (1 << 6)
#define BTN_B (1 << 7)
#define BTN_X (1 << 8)
#define BTN_Y (1 << 9)

#endif // AKIRA_BUTTONS_H
