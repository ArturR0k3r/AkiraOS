/**
 * @file akira_sim_buttons.c
 * @brief Akira Console Button Simulation
 *
 * Simulates the physical buttons with mouse and keyboard input
 */

#include <zephyr/logging/log.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>
#include "akira_sim.h"

LOG_MODULE_REGISTER(akira_sim_buttons, LOG_LEVEL_INF);

/* Button layout based on the Akira Console photo */
static akira_button_t buttons[SIM_NUM_BUTTONS] = {
    /* Power button (top right) */
    {350, 100, 20, false, AKIRA_BTN_POWER, "PWR"},

    /* Settings button (top left) */
    {50, 100, 20, false, AKIRA_BTN_SETTINGS, "SET"},

    /* D-Pad (left side, cross pattern) */
    {80, 450, SIM_BUTTON_RADIUS, false, AKIRA_BTN_UP, "↑"},
    {80, 520, SIM_BUTTON_RADIUS, false, AKIRA_BTN_DOWN, "↓"},
    {45, 485, SIM_BUTTON_RADIUS, false, AKIRA_BTN_LEFT, "←"},
    {115, 485, SIM_BUTTON_RADIUS, false, AKIRA_BTN_RIGHT, "→"},

    /* Action buttons (right side, diamond pattern) */
    {320, 450, SIM_BUTTON_RADIUS, false, AKIRA_BTN_X, "X"},
    {320, 520, SIM_BUTTON_RADIUS, false, AKIRA_BTN_B, "B"},
    {285, 485, SIM_BUTTON_RADIUS, false, AKIRA_BTN_Y, "Y"},
    {355, 485, SIM_BUTTON_RADIUS, false, AKIRA_BTN_A, "A"}};

/* Current button state bitmask */
static uint32_t button_state = 0;

/* Keyboard mapping */
static const struct
{
    SDL_Keycode key;
    akira_button_id_t button;
} key_mapping[] = {
    {SDLK_ESCAPE, AKIRA_BTN_POWER},
    {SDLK_RETURN, AKIRA_BTN_SETTINGS},
    {SDLK_w, AKIRA_BTN_UP},
    {SDLK_s, AKIRA_BTN_DOWN},
    {SDLK_a, AKIRA_BTN_LEFT},
    {SDLK_d, AKIRA_BTN_RIGHT},
    {SDLK_i, AKIRA_BTN_X},
    {SDLK_k, AKIRA_BTN_B},
    {SDLK_j, AKIRA_BTN_Y},
    {SDLK_l, AKIRA_BTN_A}};

int akira_sim_buttons_init(void)
{
    button_state = 0;

    LOG_INF("Button simulation initialized");
    LOG_INF("Keyboard controls:");
    LOG_INF("  WASD - D-Pad");
    LOG_INF("  IJKL - Action buttons (X/B/Y/A)");
    LOG_INF("  ESC  - Power button");
    LOG_INF("  ENTER - Settings");

    return 0;
}

static bool point_in_circle(int px, int py, int cx, int cy, int radius)
{
    int dx = px - cx;
    int dy = py - cy;
    return (dx * dx + dy * dy) <= (radius * radius);
}

void akira_sim_buttons_handle_mouse(int x, int y, bool pressed)
{
    for (int i = 0; i < SIM_NUM_BUTTONS; i++)
    {
        if (point_in_circle(x, y, buttons[i].x, buttons[i].y, buttons[i].radius))
        {
            buttons[i].pressed = pressed;

            if (pressed)
            {
                button_state |= (1 << buttons[i].id);
                LOG_DBG("Button %s pressed (mouse)", buttons[i].label);
            }
            else
            {
                button_state &= ~(1 << buttons[i].id);
                LOG_DBG("Button %s released (mouse)", buttons[i].label);
            }
            break;
        }
    }
}

void akira_sim_buttons_handle_keyboard(SDL_Keycode key, bool pressed)
{
    for (size_t i = 0; i < sizeof(key_mapping) / sizeof(key_mapping[0]); i++)
    {
        if (key_mapping[i].key == key)
        {
            akira_button_id_t btn_id = key_mapping[i].button;
            buttons[btn_id].pressed = pressed;

            if (pressed)
            {
                button_state |= (1 << btn_id);
                LOG_DBG("Button %s pressed (keyboard)", buttons[btn_id].label);
            }
            else
            {
                button_state &= ~(1 << btn_id);
                LOG_DBG("Button %s released (keyboard)", buttons[btn_id].label);
            }
            break;
        }
    }
}

uint32_t akira_sim_buttons_get_state(void)
{
    return button_state;
}

void akira_sim_buttons_render(SDL_Renderer *renderer)
{
    if (!renderer)
    {
        return;
    }

    /* Draw button labels/icons */
    for (int i = 0; i < SIM_NUM_BUTTONS; i++)
    {
        akira_button_t *btn = &buttons[i];

        /* Button color: pressed = yellow, released = white/gray */
        if (btn->pressed)
        {
            SDL_SetRenderDrawColor(renderer, 255, 220, 0, 255);
        }
        else
        {
            SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
        }

        /* Draw filled circle (approximate with filled rect) */
        for (int dy = -btn->radius; dy <= btn->radius; dy++)
        {
            for (int dx = -btn->radius; dx <= btn->radius; dx++)
            {
                if (dx * dx + dy * dy <= btn->radius * btn->radius)
                {
                    SDL_RenderDrawPoint(renderer, btn->x + dx, btn->y + dy);
                }
            }
        }

        /* Draw button outline */
        if (btn->pressed)
        {
            SDL_SetRenderDrawColor(renderer, 255, 180, 0, 255);
        }
        else
        {
            SDL_SetRenderDrawColor(renderer, 100, 100, 100, 255);
        }

        /* Draw circle outline (approximation) */
        for (int angle = 0; angle < 360; angle += 5)
        {
            int x = btn->x + btn->radius * cos(angle * M_PI / 180.0);
            int y = btn->y + btn->radius * sin(angle * M_PI / 180.0);
            SDL_RenderDrawPoint(renderer, x, y);
        }
    }

    /* Draw PCB background */
    SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
    SDL_Rect pcb = {10, 420, 380, 170};
    SDL_RenderFillRect(renderer, &pcb);

    /* Draw "AKIRA" logo area */
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_Rect logo = {160, 440, 80, 40};
    SDL_RenderDrawRect(renderer, &logo);
}
