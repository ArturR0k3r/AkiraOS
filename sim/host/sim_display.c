/*
 * AkiraConsole Simulator — Display host
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Maintains a 320×240 RGB565 framebuffer and converts it to RGBA8888 for
 * SDL2 display.  The drawing primitives are independent of Zephyr and match
 * the akira_display_api semantics.
 */

#include "sim_display.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

/* -----------------------------------------------------------------------
 * Framebuffer
 * --------------------------------------------------------------------- */

static uint16_t  g_fb[DISP_W * DISP_H];         /* RGB565 working buffer */
static uint32_t  g_px[DISP_W * DISP_H];         /* RGBA8888 for SDL      */
static SDL_Texture *g_tex = NULL;

/* -----------------------------------------------------------------------
 * Embedded 5×7 bitmap font (ASCII 32–126)
 * Each character is 5 bytes wide × 7 bits tall (bit 7 = top row).
 * --------------------------------------------------------------------- */

static const uint8_t FONT5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* '"' */
    {0x14,0x7F,0x14,0x7F,0x14}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$' */
    {0x23,0x13,0x08,0x64,0x62}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* '&' */
    {0x00,0x05,0x03,0x00,0x00}, /* ''' */
    {0x00,0x1C,0x22,0x41,0x00}, /* '(' */
    {0x00,0x41,0x22,0x1C,0x00}, /* ')' */
    {0x14,0x08,0x3E,0x08,0x14}, /* '*' */
    {0x08,0x08,0x3E,0x08,0x08}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* ',' */
    {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* '.' */
    {0x20,0x10,0x08,0x04,0x02}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0' */
    {0x00,0x42,0x7F,0x40,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* '2' */
    {0x21,0x41,0x45,0x4B,0x31}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4' */
    {0x27,0x45,0x45,0x45,0x39}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6' */
    {0x01,0x71,0x09,0x05,0x03}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* '8' */
    {0x06,0x49,0x49,0x29,0x1E}, /* '9' */
    {0x00,0x36,0x36,0x00,0x00}, /* ':' */
    {0x00,0x56,0x36,0x00,0x00}, /* ';' */
    {0x08,0x14,0x22,0x41,0x00}, /* '<' */
    {0x14,0x14,0x14,0x14,0x14}, /* '=' */
    {0x00,0x41,0x22,0x14,0x08}, /* '>' */
    {0x02,0x01,0x51,0x09,0x06}, /* '?' */
    {0x32,0x49,0x79,0x41,0x3E}, /* '@' */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 'A' */
    {0x7F,0x49,0x49,0x49,0x36}, /* 'B' */
    {0x3E,0x41,0x41,0x41,0x22}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D' */
    {0x7F,0x49,0x49,0x49,0x41}, /* 'E' */
    {0x7F,0x09,0x09,0x09,0x01}, /* 'F' */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 'G' */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 'H' */
    {0x00,0x41,0x7F,0x41,0x00}, /* 'I' */
    {0x20,0x40,0x41,0x3F,0x01}, /* 'J' */
    {0x7F,0x08,0x14,0x22,0x41}, /* 'K' */
    {0x7F,0x40,0x40,0x40,0x40}, /* 'L' */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N' */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 'O' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P' */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R' */
    {0x46,0x49,0x49,0x49,0x31}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T' */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 'U' */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 'V' */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 'W' */
    {0x63,0x14,0x08,0x14,0x63}, /* 'X' */
    {0x07,0x08,0x70,0x08,0x07}, /* 'Y' */
    {0x61,0x51,0x49,0x45,0x43}, /* 'Z' */
    {0x00,0x7F,0x41,0x41,0x00}, /* '[' */
    {0x02,0x04,0x08,0x10,0x20}, /* '\' */
    {0x00,0x41,0x41,0x7F,0x00}, /* ']' */
    {0x04,0x02,0x01,0x02,0x04}, /* '^' */
    {0x40,0x40,0x40,0x40,0x40}, /* '_' */
    {0x00,0x01,0x02,0x04,0x00}, /* '`' */
    {0x20,0x54,0x54,0x54,0x78}, /* 'a' */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b' */
    {0x38,0x44,0x44,0x44,0x20}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd' */
    {0x38,0x54,0x54,0x54,0x18}, /* 'e' */
    {0x08,0x7E,0x09,0x01,0x02}, /* 'f' */
    {0x0C,0x52,0x52,0x52,0x3E}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h' */
    {0x00,0x44,0x7D,0x40,0x00}, /* 'i' */
    {0x20,0x40,0x44,0x3D,0x00}, /* 'j' */
    {0x7F,0x10,0x28,0x44,0x00}, /* 'k' */
    {0x00,0x41,0x7F,0x40,0x00}, /* 'l' */
    {0x7C,0x04,0x18,0x04,0x78}, /* 'm' */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n' */
    {0x38,0x44,0x44,0x44,0x38}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p' */
    {0x08,0x14,0x14,0x18,0x7C}, /* 'q' */
    {0x7C,0x08,0x04,0x04,0x08}, /* 'r' */
    {0x48,0x54,0x54,0x54,0x20}, /* 's' */
    {0x04,0x3F,0x44,0x40,0x20}, /* 't' */
    {0x3C,0x40,0x40,0x40,0x7C}, /* 'u' */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 'v' */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 'w' */
    {0x44,0x28,0x10,0x28,0x44}, /* 'x' */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 'y' */
    {0x44,0x64,0x54,0x4C,0x44}, /* 'z' */
    {0x00,0x08,0x36,0x41,0x00}, /* '{' */
    {0x00,0x00,0x7F,0x00,0x00}, /* '|' */
    {0x00,0x41,0x36,0x08,0x00}, /* '}' */
    {0x10,0x08,0x08,0x10,0x08}, /* '~' */
};

#define FONT_W 5
#define FONT_H 7
#define FONT_SPACING 1  /* gap between characters */

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

static inline uint32_t rgb565_to_rgba(uint16_t c)
{
    uint8_t r = ((c >> 11) & 0x1F) << 3;  r |= r >> 5;
    uint8_t g = ((c >>  5) & 0x3F) << 2;  g |= g >> 6;
    uint8_t b = ( c        & 0x1F) << 3;  b |= b >> 5;
    return (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline void put_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= DISP_W || y < 0 || y >= DISP_H) return;
    g_fb[y * DISP_W + x] = color;
}

/* -----------------------------------------------------------------------
 * Init / cleanup / render
 * --------------------------------------------------------------------- */

int sim_display_init(SDL_Renderer *renderer)
{
    g_tex = SDL_CreateTexture(renderer,
                              SDL_PIXELFORMAT_RGBA8888,
                              SDL_TEXTUREACCESS_STREAMING,
                              DISP_W, DISP_H);
    if (!g_tex) return -1;
    memset(g_fb, 0, sizeof(g_fb));
    return 0;
}

void sim_display_cleanup(void)
{
    if (g_tex) { SDL_DestroyTexture(g_tex); g_tex = NULL; }
}

void sim_display_render(SDL_Renderer *renderer, const SDL_Rect *dst_rect)
{
    if (!g_tex) return;
    for (int i = 0; i < DISP_W * DISP_H; i++) {
        g_px[i] = rgb565_to_rgba(g_fb[i]);
    }
    SDL_UpdateTexture(g_tex, NULL, g_px, DISP_W * 4);
    SDL_RenderCopy(renderer, g_tex, NULL, dst_rect);
}

/* -----------------------------------------------------------------------
 * Drawing primitives
 * --------------------------------------------------------------------- */

void sim_display_clear(uint16_t color)
{
    for (int i = 0; i < DISP_W * DISP_H; i++) g_fb[i] = color;
}

void sim_display_pixel(int x, int y, uint16_t color)
{
    put_pixel(x, y, color);
}

void sim_display_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            put_pixel(col, row, color);
}

void sim_display_rect_outline(int x, int y, int w, int h, uint16_t color)
{
    sim_display_hline(x, y,       w, color);
    sim_display_hline(x, y+h-1,   w, color);
    sim_display_vline(x,     y,   h, color);
    sim_display_vline(x+w-1, y,   h, color);
}

void sim_display_hline(int x, int y, int len, uint16_t color)
{
    for (int i = 0; i < len; i++) put_pixel(x + i, y, color);
}

void sim_display_vline(int x, int y, int len, uint16_t color)
{
    for (int i = 0; i < len; i++) put_pixel(x, y + i, color);
}

void sim_display_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2;
    while (1) {
        put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

void sim_display_circle(int cx, int cy, int r, uint16_t color)
{
    int x = 0, y = r, d = 3 - 2 * r;
    while (x <= y) {
        put_pixel(cx+x, cy+y, color); put_pixel(cx-x, cy+y, color);
        put_pixel(cx+x, cy-y, color); put_pixel(cx-x, cy-y, color);
        put_pixel(cx+y, cy+x, color); put_pixel(cx-y, cy+x, color);
        put_pixel(cx+y, cy-x, color); put_pixel(cx-y, cy-x, color);
        d += (d < 0) ? 4*x+6 : 4*(x-y--)+10;
        x++;
    }
}

void sim_display_circle_fill(int cx, int cy, int r, uint16_t color)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x*x + y*y <= r*r)
                put_pixel(cx+x, cy+y, color);
}

void sim_display_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                           uint16_t color)
{
    sim_display_line(x0, y0, x1, y1, color);
    sim_display_line(x1, y1, x2, y2, color);
    sim_display_line(x2, y2, x0, y0, color);
}

void sim_display_triangle_fill(int x0, int y0, int x1, int y1, int x2, int y2,
                                uint16_t color)
{
    /* Sort vertices by y */
    if (y0 > y1) { int t; t=y0;y0=y1;y1=t; t=x0;x0=x1;x1=t; }
    if (y0 > y2) { int t; t=y0;y0=y2;y2=t; t=x0;x0=x2;x2=t; }
    if (y1 > y2) { int t; t=y1;y1=y2;y2=t; t=x1;x1=x2;x2=t; }
    int total = y2 - y0;
    for (int y = y0; y <= y2; y++) {
        int first = (y - y0);
        int xA = x0 + (x2 - x0) * first / (total ? total : 1);
        int xB = (y < y1)
            ? x0 + (x1 - x0) * first / (y1 - y0 ? y1 - y0 : 1)
            : x1 + (x2 - x1) * (y - y1) / (y2 - y1 ? y2 - y1 : 1);
        if (xA > xB) { int t = xA; xA = xB; xB = t; }
        sim_display_hline(xA, y, xB - xA + 1, color);
    }
}

void sim_display_rounded_rect(int x, int y, int w, int h, int r, uint16_t color)
{
    sim_display_hline(x+r,   y,       w-2*r, color);
    sim_display_hline(x+r,   y+h-1,   w-2*r, color);
    sim_display_vline(x,     y+r,     h-2*r, color);
    sim_display_vline(x+w-1, y+r,     h-2*r, color);
    /* corners — approximate with quarter circles */
    for (int a = 0; a <= 90; a += 5) {
        int dx = (int)(r * cos(a * M_PI / 180.0));
        int dy = (int)(r * sin(a * M_PI / 180.0));
        put_pixel(x+r   - dx, y+r   - dy, color);
        put_pixel(x+w-r + dx, y+r   - dy, color);
        put_pixel(x+r   - dx, y+h-r + dy, color);
        put_pixel(x+w-r + dx, y+h-r + dy, color);
    }
}

void sim_display_rounded_rect_fill(int x, int y, int w, int h, int r,
                                    uint16_t color)
{
    sim_display_rect(x+r, y,   w-2*r, h, color);
    sim_display_rect(x,   y+r, r,     h-2*r, color);
    sim_display_rect(x+w-r, y+r, r,   h-2*r, color);
    sim_display_circle_fill(x+r,   y+r,   r, color);
    sim_display_circle_fill(x+w-r, y+r,   r, color);
    sim_display_circle_fill(x+r,   y+h-r, r, color);
    sim_display_circle_fill(x+w-r, y+h-r, r, color);
}

/* -----------------------------------------------------------------------
 * Text rendering (5×7 font, scale 1 for normal, scale 2 for large)
 * --------------------------------------------------------------------- */

static void draw_char(int x, int y, char c, uint16_t color, int scale)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = FONT5x7[c - 32];
    for (int col = 0; col < FONT_W; col++) {
        for (int row = 0; row < FONT_H; row++) {
            if (glyph[col] & (1 << row)) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        put_pixel(x + col*scale + sx,
                                  y + row*scale + sy, color);
            }
        }
    }
}

void sim_display_text(int x, int y, const char *text, uint16_t color)
{
    if (!text) return;
    int cx = x;
    for (; *text; text++) {
        if (*text == '\n') { cx = x; y += FONT_H + FONT_SPACING + 1; continue; }
        draw_char(cx, y, *text, color, 1);
        cx += FONT_W + FONT_SPACING;
    }
}

void sim_display_text_large(int x, int y, const char *text, uint16_t color)
{
    if (!text) return;
    int cx = x;
    for (; *text; text++) {
        if (*text == '\n') { cx = x; y += (FONT_H + FONT_SPACING) * 2; continue; }
        draw_char(cx, y, *text, color, 2);
        cx += (FONT_W + FONT_SPACING) * 2;
    }
}

void sim_display_number(int x, int y, int32_t value, uint16_t color)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)value);
    sim_display_text(x, y, buf, color);
}

void sim_display_progress_bar(int x, int y, int w, int h,
                               int32_t value, int32_t max_val,
                               uint16_t fg, uint16_t bg)
{
    sim_display_rect(x, y, w, h, bg);
    if (max_val > 0 && value > 0) {
        int fill = (int)((long)w * value / max_val);
        if (fill > w) fill = w;
        sim_display_rect(x, y, fill, h, fg);
    }
}

void sim_display_bitmap(int x, int y, int w, int h, const uint16_t *data)
{
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            put_pixel(x + col, y + row, data[row * w + col]);
}

void sim_display_bitmap_transparent(int x, int y, int w, int h,
                                     const uint16_t *data, uint16_t key)
{
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++) {
            uint16_t px = data[row * w + col];
            if (px != key) put_pixel(x + col, y + row, px);
        }
}
