/*
 * AkiraConsole Simulator — Display host
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef SIM_DISPLAY_H
#define SIM_DISPLAY_H

#include <stdint.h>
#include <SDL2/SDL.h>

/* Console display resolution (matches ILI9341 hardware) */
#define DISP_W 320
#define DISP_H 240

/**
 * @brief Initialise the display framebuffer and SDL texture.
 * @param renderer  Active SDL renderer.
 * @return 0 on success, -1 on failure.
 */
int sim_display_init(SDL_Renderer *renderer);

/** @brief Free SDL texture resources. */
void sim_display_cleanup(void);

/**
 * @brief Push the current framebuffer to the SDL texture and render it.
 * @param renderer  Active SDL renderer.
 * @param dst_rect  Where to draw the display in the window.
 */
void sim_display_render(SDL_Renderer *renderer, const SDL_Rect *dst_rect);

/* -----------------------------------------------------------------------
 * Framebuffer drawing primitives (RGB565, same API as akira_display_api)
 * All writes go to the internal framebuffer.  Call sim_display_render()
 * to push to screen.
 * --------------------------------------------------------------------- */

void sim_display_clear(uint16_t color);
void sim_display_pixel(int x, int y, uint16_t color);
void sim_display_rect(int x, int y, int w, int h, uint16_t color);
void sim_display_rect_outline(int x, int y, int w, int h, uint16_t color);
void sim_display_hline(int x, int y, int len, uint16_t color);
void sim_display_vline(int x, int y, int len, uint16_t color);
void sim_display_line(int x0, int y0, int x1, int y1, uint16_t color);
void sim_display_circle(int cx, int cy, int r, uint16_t color);
void sim_display_circle_fill(int cx, int cy, int r, uint16_t color);
void sim_display_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);
void sim_display_triangle_fill(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);
void sim_display_text(int x, int y, const char *text, uint16_t color);
void sim_display_text_large(int x, int y, const char *text, uint16_t color);
void sim_display_number(int x, int y, int32_t value, uint16_t color);
void sim_display_progress_bar(int x, int y, int w, int h,
                               int32_t value, int32_t max_val,
                               uint16_t fg, uint16_t bg);
void sim_display_rounded_rect(int x, int y, int w, int h, int radius, uint16_t color);
void sim_display_rounded_rect_fill(int x, int y, int w, int h, int radius, uint16_t color);
void sim_display_bitmap(int x, int y, int w, int h, const uint16_t *data);
void sim_display_bitmap_transparent(int x, int y, int w, int h,
                                     const uint16_t *data, uint16_t key);

#endif /* SIM_DISPLAY_H */
