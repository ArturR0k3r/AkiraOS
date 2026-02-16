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

    /* Draw console lower section (button area) with darker background */
    SDL_SetRenderDrawColor(renderer, 25, 25, 30, 255);
    SDL_Rect button_area = {10, 410, 380, 180};
    SDL_RenderFillRect(renderer, &button_area);

    /* Draw grid pattern on button area for texture */
    SDL_SetRenderDrawColor(renderer, 35, 35, 40, 255);
    for (int x = 15; x < 390; x += 20)
    {
        SDL_RenderDrawLine(renderer, x, 415, x, 585);
    }
    for (int y = 415; y < 590; y += 20)
    {
        SDL_RenderDrawLine(renderer, 15, y, 385, y);
    }

    /* Draw button labels/icons with 3D effect */
    for (int i = 0; i < SIM_NUM_BUTTONS; i++)
    {
        akira_button_t *btn = &buttons[i];

        /* Determine button color based on type and state */
        int r, g, b;
        int shadow_r, shadow_g, shadow_b;
        int highlight_r, highlight_g, highlight_b;

        if (btn->id == AKIRA_BTN_POWER)
        {
            /* Power button - red */
            r = btn->pressed ? 255 : 200;
            g = btn->pressed ? 50 : 30;
            b = btn->pressed ? 50 : 30;
            shadow_r = 100; shadow_g = 10; shadow_b = 10;
            highlight_r = 255; highlight_g = 100; highlight_b = 100;
        }
        else if (btn->id == AKIRA_BTN_SETTINGS)
        {
            /* Settings button - gray */
            r = btn->pressed ? 200 : 150;
            g = btn->pressed ? 200 : 150;
            b = btn->pressed ? 200 : 150;
            shadow_r = 80; shadow_g = 80; shadow_b = 80;
            highlight_r = 220; highlight_g = 220; highlight_b = 220;
        }
        else if (btn->id >= AKIRA_BTN_UP && btn->id <= AKIRA_BTN_RIGHT)
        {
            /* D-Pad - dark gray */
            r = btn->pressed ? 120 : 80;
            g = btn->pressed ? 120 : 80;
            b = btn->pressed ? 120 : 80;
            shadow_r = 40; shadow_g = 40; shadow_b = 40;
            highlight_r = 150; highlight_g = 150; highlight_b = 150;
        }
        else
        {
            /* Action buttons - colorful */
            switch (btn->id)
            {
            case AKIRA_BTN_A:
                r = btn->pressed ? 100 : 60; g = btn->pressed ? 255 : 200; b = btn->pressed ? 100 : 60;
                shadow_r = 30; shadow_g = 120; shadow_b = 30;
                highlight_r = 150; highlight_g = 255; highlight_b = 150;
                break;
            case AKIRA_BTN_B:
                r = btn->pressed ? 255 : 200; g = btn->pressed ? 100 : 60; b = btn->pressed ? 100 : 60;
                shadow_r = 120; shadow_g = 30; shadow_b = 30;
                highlight_r = 255; highlight_g = 150; highlight_b = 150;
                break;
            case AKIRA_BTN_X:
                r = btn->pressed ? 100 : 60; g = btn->pressed ? 150 : 100; b = btn->pressed ? 255 : 200;
                shadow_r = 30; shadow_g = 60; shadow_b = 120;
                highlight_r = 150; highlight_g = 200; highlight_b = 255;
                break;
            case AKIRA_BTN_Y:
                r = btn->pressed ? 255 : 200; g = btn->pressed ? 255 : 200; b = btn->pressed ? 100 : 60;
                shadow_r = 120; shadow_g = 120; shadow_b = 30;
                highlight_r = 255; highlight_g = 255; highlight_b = 150;
                break;
            default:
                r = 150; g = 150; b = 150;
                shadow_r = 70; shadow_g = 70; shadow_b = 70;
                highlight_r = 200; highlight_g = 200; highlight_b = 200;
            }
        }

        /* Draw shadow (bottom-right) for 3D effect */
        if (!btn->pressed)
        {
            SDL_SetRenderDrawColor(renderer, shadow_r, shadow_g, shadow_b, 255);
            for (int dy = -btn->radius + 2; dy <= btn->radius + 2; dy++)
            {
                for (int dx = -btn->radius + 2; dx <= btn->radius + 2; dx++)
                {
                    if (dx * dx + dy * dy <= (btn->radius + 2) * (btn->radius + 2))
                    {
                        SDL_RenderDrawPoint(renderer, btn->x + dx + 2, btn->y + dy + 2);
                    }
                }
            }
        }

        /* Draw main button body */
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        for (int dy = -btn->radius; dy <= btn->radius; dy++)
        {
            for (int dx = -btn->radius; dx <= btn->radius; dx++)
            {
                int dist_sq = dx * dx + dy * dy;
                if (dist_sq <= btn->radius * btn->radius)
                {
                    /* Add gradient effect */
                    float factor = 1.0f - (float)dist_sq / (btn->radius * btn->radius);
                    int adj_r = r + (int)(factor * 20);
                    int adj_g = g + (int)(factor * 20);
                    int adj_b = b + (int)(factor * 20);
                    adj_r = adj_r > 255 ? 255 : adj_r;
                    adj_g = adj_g > 255 ? 255 : adj_g;
                    adj_b = adj_b > 255 ? 255 : adj_b;
                    SDL_SetRenderDrawColor(renderer, adj_r, adj_g, adj_b, 255);
                    SDL_RenderDrawPoint(renderer, btn->x + dx, btn->y + dy);
                }
            }
        }

        /* Draw highlight (top-left) for 3D effect */
        if (!btn->pressed)
        {
            SDL_SetRenderDrawColor(renderer, highlight_r, highlight_g, highlight_b, 200);
            for (int angle = 180; angle < 360; angle += 3)
            {
                int x = btn->x + (btn->radius - 3) * cos(angle * M_PI / 180.0);
                int y = btn->y + (btn->radius - 3) * sin(angle * M_PI / 180.0);
                SDL_RenderDrawPoint(renderer, x, y);
            }
        }

        /* Draw button outline */
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        for (int angle = 0; angle < 360; angle += 3)
        {
            int x = btn->x + btn->radius * cos(angle * M_PI / 180.0);
            int y = btn->y + btn->radius * sin(angle * M_PI / 180.0);
            SDL_RenderDrawPoint(renderer, x, y);
        }
    }

    /* Draw decorative elements */
    /* Left side accent line */
    SDL_SetRenderDrawColor(renderer, 180, 20, 20, 255);
    SDL_RenderDrawLine(renderer, 15, 420, 15, 580);
    SDL_RenderDrawLine(renderer, 16, 420, 16, 580);

    /* Right side accent line */
    SDL_RenderDrawLine(renderer, 384, 420, 384, 580);
    SDL_RenderDrawLine(renderer, 385, 420, 385, 580);

    /* Bottom accent line */
    SDL_RenderDrawLine(renderer, 15, 584, 385, 584);
    SDL_RenderDrawLine(renderer, 15, 585, 385, 585);
}
