/**
 * @file akira_sim_display.c
 * @brief Akira Console Display Simulation
 *
 * Simulates the ILI9341 TFT display in SDL2 window
 */

#include <zephyr/logging/log.h>
#include <SDL2/SDL.h>
#include "akira_sim.h"

LOG_MODULE_REGISTER(akira_sim_display, LOG_LEVEL_INF);

/* SDL display texture and surface */
static SDL_Texture *display_texture = NULL;
static SDL_Rect display_rect;

/* Cached framebuffer for rendering */
static uint32_t display_pixels[SIM_DISPLAY_WIDTH * SIM_DISPLAY_HEIGHT];

/**
 * @brief Convert RGB565 to RGB888 for SDL
 */
static inline uint32_t rgb565_to_rgb888(uint16_t color)
{
    uint8_t r = ((color >> 11) & 0x1F) << 3;
    uint8_t g = ((color >> 5) & 0x3F) << 2;
    uint8_t b = (color & 0x1F) << 3;

    /* Expand to full 8-bit range */
    r |= (r >> 5);
    g |= (g >> 6);
    b |= (b >> 5);

    return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

int akira_sim_display_init(SDL_Renderer *renderer)
{
    if (!renderer)
    {
        LOG_ERR("Invalid renderer");
        return -1;
    }

    /* Create texture for display */
    display_texture = SDL_CreateTexture(renderer,
                                        SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        SIM_DISPLAY_WIDTH,
                                        SIM_DISPLAY_HEIGHT);

    if (!display_texture)
    {
        LOG_ERR("Failed to create display texture: %s", SDL_GetError());
        return -1;
    }

    /* Set display position and size in window */
    display_rect.x = SIM_DISPLAY_X;
    display_rect.y = SIM_DISPLAY_Y;
    display_rect.w = SIM_DISPLAY_WIDTH;
    display_rect.h = SIM_DISPLAY_HEIGHT;

    /* Initialize display to black */
    memset(display_pixels, 0, sizeof(display_pixels));

    LOG_INF("Display simulation initialized (%dx%d)",
            SIM_DISPLAY_WIDTH, SIM_DISPLAY_HEIGHT);

    return 0;
}

void akira_sim_display_cleanup(void)
{
    if (display_texture)
    {
        SDL_DestroyTexture(display_texture);
        display_texture = NULL;
    }
}

void akira_sim_display_update(SDL_Renderer *renderer, const uint16_t *framebuffer)
{
    if (!renderer || !display_texture || !framebuffer)
    {
        return;
    }

    /* Convert RGB565 framebuffer to RGB888 for SDL */
    for (int i = 0; i < SIM_DISPLAY_WIDTH * SIM_DISPLAY_HEIGHT; i++)
    {
        display_pixels[i] = rgb565_to_rgb888(framebuffer[i]);
    }

    /* Update texture */
    SDL_UpdateTexture(display_texture, NULL, display_pixels,
                      SIM_DISPLAY_WIDTH * sizeof(uint32_t));
}

void akira_sim_display_render(SDL_Renderer *renderer)
{
    if (!renderer || !display_texture)
    {
        return;
    }

    /* Draw outer bezel frame (dark with red accent) */
    SDL_SetRenderDrawColor(renderer, 15, 15, 20, 255);
    SDL_Rect outer_bezel = {
        display_rect.x - 15,
        display_rect.y - 15,
        display_rect.w + 30,
        display_rect.h + 30};
    SDL_RenderFillRect(renderer, &outer_bezel);

    /* Draw red accent border around bezel */
    SDL_SetRenderDrawColor(renderer, 200, 30, 30, 255);
    SDL_RenderDrawRect(renderer, &outer_bezel);
    SDL_Rect outer_border2 = {
        display_rect.x - 14,
        display_rect.y - 14,
        display_rect.w + 28,
        display_rect.h + 28};
    SDL_RenderDrawRect(renderer, &outer_border2);

    /* Draw inner bezel (slightly lighter) */
    SDL_SetRenderDrawColor(renderer, 25, 25, 30, 255);
    SDL_Rect inner_bezel = {
        display_rect.x - 8,
        display_rect.y - 8,
        display_rect.w + 16,
        display_rect.h + 16};
    SDL_RenderFillRect(renderer, &inner_bezel);

    /* Draw display frame border */
    SDL_SetRenderDrawColor(renderer, 40, 40, 45, 255);
    SDL_Rect frame = {
        display_rect.x - 2,
        display_rect.y - 2,
        display_rect.w + 4,
        display_rect.h + 4};
    SDL_RenderFillRect(renderer, &frame);

    /* Draw the display content */
    SDL_RenderCopy(renderer, display_texture, NULL, &display_rect);

    /* Add screen glare effect (subtle white gradient on top-left) */
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 15);
    for (int y = 0; y < 80; y++)
    {
        for (int x = 0; x < 80; x++)
        {
            if (x + y < 80)
            {
                SDL_RenderDrawPoint(renderer,
                    display_rect.x + x,
                    display_rect.y + y);
            }
        }
    }
}
