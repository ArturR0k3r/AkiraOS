/*
 * AkiraConsole Simulator — Input host
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Maps keyboard keys to AkiraConsole button codes and maintains a button
 * bitmask + edge-event ring buffer.
 *
 * Keyboard layout:
 *   W/A/S/D  — D-pad up/left/down/right
 *   Arrow keys — D-pad (alternative)
 *   Z        — A button (confirm)
 *   X        — B button (back)
 *   Q        — X button (HOME)
 *   C        — Y button (context)
 */

#include "sim_input.h"
#include <string.h>
#include <stdatomic.h>

#define EVENT_QUEUE_SIZE 64

static atomic_uint g_bitmask;

/* Simple ring buffer for edge events */
static sim_input_event_t g_queue[EVENT_QUEUE_SIZE];
static int g_qhead;
static int g_qtail;

/* Keyboard → button code mapping */
static const struct {
    SDL_Keycode key;
    int         code;
} KEY_MAP[] = {
    { SDLK_w,      SIM_BTN_UP    },
    { SDLK_UP,     SIM_BTN_UP    },
    { SDLK_s,      SIM_BTN_DOWN  },
    { SDLK_DOWN,   SIM_BTN_DOWN  },
    { SDLK_a,      SIM_BTN_LEFT  },
    { SDLK_LEFT,   SIM_BTN_LEFT  },
    { SDLK_d,      SIM_BTN_RIGHT },
    { SDLK_RIGHT,  SIM_BTN_RIGHT },
    { SDLK_z,      SIM_BTN_A     },
    { SDLK_RETURN, SIM_BTN_A     },
    { SDLK_x,      SIM_BTN_B     },
    { SDLK_ESCAPE, SIM_BTN_B     },
    { SDLK_q,      SIM_BTN_X     }, /* HOME */
    { SDLK_c,      SIM_BTN_Y     },
};
#define KEY_MAP_LEN (int)(sizeof(KEY_MAP)/sizeof(KEY_MAP[0]))

void sim_input_init(void)
{
    atomic_store(&g_bitmask, 0);
    g_qhead = g_qtail = 0;
    memset(g_queue, 0, sizeof(g_queue));
}

void sim_input_handle_key(SDL_Keycode key, bool pressed)
{
    for (int i = 0; i < KEY_MAP_LEN; i++) {
        if (KEY_MAP[i].key != key) continue;
        int code = KEY_MAP[i].code;

        /* Update bitmask atomically */
        uint32_t mask = (uint32_t)(1u << code);
        if (pressed) {
            atomic_fetch_or(&g_bitmask, mask);
        } else {
            atomic_fetch_and(&g_bitmask, ~mask);
        }

        /* Enqueue edge event */
        int next = (g_qtail + 1) % EVENT_QUEUE_SIZE;
        if (next != g_qhead) {
            g_queue[g_qtail] = (sim_input_event_t){
                .button_id = (uint32_t)code,
                .pressed   = pressed ? 1u : 0u,
            };
            g_qtail = next;
        }
        break;
    }
}

uint32_t sim_input_get_bitmask(void)
{
    return atomic_load(&g_bitmask);
}

bool sim_input_poll_event(sim_input_event_t *out)
{
    if (g_qhead == g_qtail) return false;
    *out = g_queue[g_qhead];
    g_qhead = (g_qhead + 1) % EVENT_QUEUE_SIZE;
    return true;
}
