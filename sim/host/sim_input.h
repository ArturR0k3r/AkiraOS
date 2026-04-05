/*
 * AkiraConsole Simulator — Input host
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef SIM_INPUT_H
#define SIM_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

/*
 * Button IDs match the zephyr,code values in the hardware DTS overlay and
 * the AKIRA_BTN_* constants in akira_input_api.h.
 * Bit N of the bitmask = button with code N is held.
 */
#define SIM_BTN_UP    2
#define SIM_BTN_DOWN  3
#define SIM_BTN_LEFT  4
#define SIM_BTN_RIGHT 5
#define SIM_BTN_A     6   /* Confirm / launch        */
#define SIM_BTN_B     7   /* Back / cancel           */
#define SIM_BTN_X     8   /* HOME (long-press = home)*/
#define SIM_BTN_Y     9   /* Context menu            */

/* Edge event (matches akira_input_event_t in the SDK) */
typedef struct {
    uint32_t button_id;
    uint32_t pressed;    /* 1 = pressed, 0 = released */
} sim_input_event_t;

void sim_input_init(void);

/** Handle SDL keyboard events. */
void sim_input_handle_key(SDL_Keycode key, bool pressed);

/** Return current button bitmask (bit N = button code N held). */
uint32_t sim_input_get_bitmask(void);

/**
 * Drain one edge event (non-blocking).
 * @return true if event was filled, false if queue empty.
 */
bool sim_input_poll_event(sim_input_event_t *out);

#endif /* SIM_INPUT_H */
