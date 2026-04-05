/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_display_api
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_display_api, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_display_api.c
 * @brief Display API: platform-agnostic primitives + WASM native exports.
 *
 * Draw primitives write into the framebuffer only.  The framebuffer is pushed
 * to the hardware driver by akira_display_flush() / akira_native_display_flush().
 * An auto-flush work item fires 50 ms after the last draw call so apps that
 * forget to call flush() still get their output shown.
 */
#include "akira_api.h"
#include <runtime/security.h>
#include <drivers/platform_hal.h>
#include "../drivers/display/fonts.h"
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <stdlib.h>  /* abs() */
#include <string.h>  /* memcpy() */

#define AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(ret_val) do {} while (0)

void akira_display_clear(uint16_t color)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    for (int y = 0; y < 320; y++)
        for (int x = 0; x < 240; x++)
            akira_sim_draw_pixel(x, y, color);
#else
    struct display_capabilities caps;
    if (akira_display_hal_get_capabilities(&caps) < 0) {
        LOG_DBG("Failed to get display capabilities");
        return;
    }

    uint16_t *fb = akira_framebuffer_get();
    if (!fb) {
        LOG_WRN("No display framebuffer available");
        return;
    }

    int pixels = caps.x_resolution * caps.y_resolution;
    for (int i = 0; i < pixels; i++)
        fb[i] = color;
#endif
}

void akira_display_pixel(int x, int y, uint16_t color)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    if (x < 0 || x >= 240 || y < 0 || y >= 320)
        return;
    akira_sim_draw_pixel(x, y, color);
#else
    struct display_capabilities caps;
    if (akira_display_hal_get_capabilities(&caps) < 0)
        return;
    
    if (x < 0 || x >= caps.x_resolution || y < 0 || y >= caps.y_resolution)
        return;

    uint16_t *fb = akira_framebuffer_get();
    if (fb)
        fb[y * caps.x_resolution + x] = color;
#endif
}

void akira_display_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0)
        return;

#if AKIRA_PLATFORM_NATIVE_SIM
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++)
            akira_sim_draw_pixel(px, py, color);
#else
    struct display_capabilities caps;
    if (akira_display_hal_get_capabilities(&caps) < 0)
        return;

    uint16_t *fb = akira_framebuffer_get();
    if (!fb)
        return;

    /* Clip to screen bounds */
    int x1 = MAX(x, 0);
    int y1 = MAX(y, 0);
    int x2 = MIN(x + w, (int)caps.x_resolution);
    int y2 = MIN(y + h, (int)caps.y_resolution);
    int span = x2 - x1;

    if (span <= 0)
        return;

    for (int py = y1; py < y2; py++) {
        uint16_t *row = fb + py * caps.x_resolution + x1;
        for (int i = 0; i < span; i++)
            row[i] = color;
    }
#endif
}

void akira_display_text(int x, int y, const char *text, uint16_t color)
{
    if (!text) return;
    draw_string(x, y, text, color, akira_display_pixel, FONT_7X10);
}

void akira_display_text_large(int x, int y, const char *text, uint16_t color)
{
    if (!text) return;
    draw_string(x, y, text, color, akira_display_pixel, FONT_11X18);
}

void akira_display_flush(void)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    akira_sim_show_display();
#else
    akira_display_hal_flush();
#endif
}

void akira_display_get_size(int *width, int *height)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    if (width) *width = 240;
    if (height) *height = 320;
#else
    struct display_capabilities caps;
    if (akira_display_hal_get_capabilities(&caps) < 0) {
        LOG_DBG("Failed to get display capabilities");
        if (width) *width = 0;
        if (height) *height = 0;
        return;
    }

    if (width) *width = caps.x_resolution;
    if (height) *height = caps.y_resolution;
#endif
}

/* ---------------------------------------------------------------------------
 * Phase 3.5 — Additional primitives
 * -------------------------------------------------------------------------*/

void akira_display_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    /* Bresenham's line algorithm — integer arithmetic only, no FPU */
    int dx =  abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (1) {
        akira_display_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void akira_display_circle(int cx, int cy, int r, uint16_t color)
{
    /* Midpoint circle algorithm — O(r) iterations */
    if (r <= 0) return;
    int x = r, y = 0, d = 1 - r;

    while (x >= y) {
        akira_display_pixel(cx + x, cy + y, color);
        akira_display_pixel(cx - x, cy + y, color);
        akira_display_pixel(cx + x, cy - y, color);
        akira_display_pixel(cx - x, cy - y, color);
        akira_display_pixel(cx + y, cy + x, color);
        akira_display_pixel(cx - y, cy + x, color);
        akira_display_pixel(cx + y, cy - x, color);
        akira_display_pixel(cx - y, cy - x, color);
        y++;
        d += (d < 0) ? 2 * y + 1 : (x--, 2 * (y - x) + 1);
    }
}

void akira_display_circle_fill(int cx, int cy, int r, uint16_t color)
{
    /* Filled circle via horizontal span fill using midpoint loop */
    if (r <= 0) return;
    int x = r, y = 0, d = 1 - r;

    while (x >= y) {
        akira_display_rect(cx - x, cy - y, 2 * x + 1, 1, color);
        akira_display_rect(cx - x, cy + y, 2 * x + 1, 1, color);
        akira_display_rect(cx - y, cy - x, 2 * y + 1, 1, color);
        akira_display_rect(cx - y, cy + x, 2 * y + 1, 1, color);
        y++;
        d += (d < 0) ? 2 * y + 1 : (x--, 2 * (y - x) + 1);
    }
}

void akira_display_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    akira_display_line(x0, y0, x1, y1, color);
    akira_display_line(x1, y1, x2, y2, color);
    akira_display_line(x2, y2, x0, y0, color);
}

void akira_display_triangle_fill(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    /* Sort vertices ascending by Y */
#define SWAP_INT(a, b) do { int _t = (a); (a) = (b); (b) = _t; } while (0)
    if (y0 > y1) { SWAP_INT(x0, x1); SWAP_INT(y0, y1); }
    if (y1 > y2) { SWAP_INT(x1, x2); SWAP_INT(y1, y2); }
    if (y0 > y1) { SWAP_INT(x0, x1); SWAP_INT(y0, y1); }
#undef SWAP_INT

    /* Scanline rasteriser — integer fixed-point interpolation */
    for (int y = y0; y <= y2; y++) {
        int xa = (y2 != y0) ? x0 + (x2 - x0) * (y - y0) / (y2 - y0) : x0;
        int xb;
        if (y <= y1)
            xb = (y1 != y0) ? x0 + (x1 - x0) * (y - y0) / (y1 - y0) : x0;
        else
            xb = (y2 != y1) ? x1 + (x2 - x1) * (y - y1) / (y2 - y1) : x1;

        if (xa > xb) { int t = xa; xa = xb; xb = t; }
        akira_display_rect(xa, y, xb - xa + 1, 1, color);
    }
}

void akira_display_rect_outline(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    akira_display_line(x,         y,         x + w - 1, y,         color);
    akira_display_line(x + w - 1, y,         x + w - 1, y + h - 1, color);
    akira_display_line(x + w - 1, y + h - 1, x,         y + h - 1, color);
    akira_display_line(x,         y + h - 1, x,         y,         color);
}

/* ---------------------------------------------------------------------------
 * Phase 4 — UI helper primitives
 * -------------------------------------------------------------------------*/

void akira_display_hline(int x, int y, int len, uint16_t color)
{
    /* Horizontal pixel run — thin wrapper over rect for clarity */
    if (len <= 0) return;
    akira_display_rect(x, y, len, 1, color);
}

void akira_display_vline(int x, int y, int len, uint16_t color)
{
    /* Vertical pixel run — implemented as individual pixels stepping by row */
    if (len <= 0) return;
    akira_display_rect(x, y, 1, len, color);
}

void akira_display_number(int x, int y, int32_t value, uint16_t color)
{
    /* Convert integer to decimal string, then render with the small font.
     * Buffer: sign + 10 digits + NUL → 12 bytes is enough for int32_t. */
    char buf[13];
    int  i = 12;
    buf[i] = '\0';

    int32_t v = value;
    int negative = (v < 0);
    if (v == 0) {
        buf[--i] = '0';
    } else {
        /* Work with positive magnitude; handle INT32_MIN edge case */
        uint32_t uv = negative ? (uint32_t)(-(v + 1)) + 1U : (uint32_t)v;
        while (uv) {
            buf[--i] = '0' + (int)(uv % 10);
            uv /= 10;
        }
    }
    if (negative)
        buf[--i] = '-';

    akira_display_text(x, y, &buf[i], color);
}

void akira_display_progress_bar(int x, int y, int w, int h,
                                 int32_t value, int32_t max_val,
                                 uint16_t fg, uint16_t bg)
{
    if (w <= 0 || h <= 0 || max_val <= 0) return;

    /* Background */
    akira_display_rect(x, y, w, h, bg);

    /* Foreground fill proportional to value/max_val */
    int32_t clamped = value < 0 ? 0 : (value > max_val ? max_val : value);
    int fill_w = (int)((int64_t)clamped * w / max_val);
    if (fill_w > 0)
        akira_display_rect(x, y, fill_w, h, fg);

    /* Border outline */
    akira_display_rect_outline(x, y, w, h, fg);
}

void akira_display_rounded_rect(int x, int y, int w, int h,
                                 int radius, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    if (radius < 0) radius = 0;

    /* Clamp radius so the arcs fit */
    int max_r = (w < h ? w : h) / 2;
    if (radius > max_r) radius = max_r;

    if (radius == 0) {
        akira_display_rect_outline(x, y, w, h, color);
        return;
    }

    /* Four straight edge segments */
    akira_display_hline(x + radius,     y,             w - 2 * radius, color); /* top    */
    akira_display_hline(x + radius,     y + h - 1,     w - 2 * radius, color); /* bottom */
    akira_display_vline(x,             y + radius,     h - 2 * radius, color); /* left   */
    akira_display_vline(x + w - 1,     y + radius,     h - 2 * radius, color); /* right  */

    /* Four corner arcs via midpoint algorithm, each restricted to its quadrant.
     * Corner centres: TL=(x+r, y+r), TR=(x+w-1-r, y+r),
     *                 BL=(x+r, y+h-1-r), BR=(x+w-1-r, y+h-1-r) */
    int cx_l = x + radius;
    int cx_r = x + w - 1 - radius;
    int cy_t = y + radius;
    int cy_b = y + h - 1 - radius;

    int px = radius, py = 0, d = 1 - radius;
    while (px >= py) {
        /* TL */ akira_display_pixel(cx_l - px, cy_t - py, color);
                akira_display_pixel(cx_l - py, cy_t - px, color);
        /* TR */ akira_display_pixel(cx_r + px, cy_t - py, color);
                akira_display_pixel(cx_r + py, cy_t - px, color);
        /* BL */ akira_display_pixel(cx_l - px, cy_b + py, color);
                akira_display_pixel(cx_l - py, cy_b + px, color);
        /* BR */ akira_display_pixel(cx_r + px, cy_b + py, color);
                akira_display_pixel(cx_r + py, cy_b + px, color);
        py++;
        d += (d < 0) ? 2 * py + 1 : (px--, 2 * (py - px) + 1);
    }
}

void akira_display_rounded_rect_fill(int x, int y, int w, int h,
                                      int radius, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    if (radius < 0) radius = 0;

    int max_r = (w < h ? w : h) / 2;
    if (radius > max_r) radius = max_r;

    if (radius == 0) {
        akira_display_rect(x, y, w, h, color);
        return;
    }

    /* Central cross: horizontal middle band + vertical side strips */
    akira_display_rect(x,            y + radius, w,            h - 2 * radius, color);

    /* Filled corner quarter-discs using midpoint scanline fill */
    int cx_l = x + radius;
    int cx_r = x + w - 1 - radius;
    int cy_t = y + radius;
    int cy_b = y + h - 1 - radius;

    int px = radius, py = 0, d = 1 - radius;
    while (px >= py) {
        /* Fill horizontal spans for top and bottom rounded caps */
        akira_display_hline(cx_l - px, cy_t - py, cx_r - cx_l + 2 * px + 1, color);
        akira_display_hline(cx_l - px, cy_b + py, cx_r - cx_l + 2 * px + 1, color);
        if (py != 0) {
            akira_display_hline(cx_l - py, cy_t - px, cx_r - cx_l + 2 * py + 1, color);
            akira_display_hline(cx_l - py, cy_b + px, cx_r - cx_l + 2 * py + 1, color);
        }
        py++;
        d += (d < 0) ? 2 * py + 1 : (px--, 2 * (py - px) + 1);
    }
}

void akira_display_bitmap(int x, int y, int w, int h, const uint16_t *data)
{
    if (!data || w <= 0 || h <= 0) return;

#if AKIRA_PLATFORM_NATIVE_SIM
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++)
            akira_sim_draw_pixel(x + px, y + py, data[py * w + px]);
#else
    struct display_capabilities caps;
    if (akira_display_hal_get_capabilities(&caps) < 0) return;
    uint16_t *fb = akira_framebuffer_get();
    if (!fb) return;

    int x1 = MAX(x, 0), y1 = MAX(y, 0);
    int x2 = MIN(x + w, (int)caps.x_resolution);
    int y2 = MIN(y + h, (int)caps.y_resolution);

    for (int py = y1; py < y2; py++) {
        int span  = x2 - x1;
        const uint16_t *src = data + (py - y) * w + (x1 - x);
        uint16_t       *dst = fb   + py * caps.x_resolution + x1;
        memcpy(dst, src, (size_t)span * sizeof(uint16_t));
    }
#endif
}

void akira_display_bitmap_transparent(int x, int y, int w, int h,
                                      const uint16_t *data, uint16_t key)
{
    if (!data || w <= 0 || h <= 0) return;

#if AKIRA_PLATFORM_NATIVE_SIM
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++) {
            uint16_t c = data[py * w + px];
            if (c != key)
                akira_sim_draw_pixel(x + px, y + py, c);
        }
#else
    struct display_capabilities caps;
    if (akira_display_hal_get_capabilities(&caps) < 0) return;
    uint16_t *fb = akira_framebuffer_get();
    if (!fb) return;

    int x1 = MAX(x, 0), y1 = MAX(y, 0);
    int x2 = MIN(x + w, (int)caps.x_resolution);
    int y2 = MIN(y + h, (int)caps.y_resolution);

    for (int py = y1; py < y2; py++) {
        for (int px = x1; px < x2; px++) {
            uint16_t c = data[(py - y) * w + (px - x)];
            if (c != key)
                fb[py * caps.x_resolution + px] = c;
        }
    }
#endif
}

/* ---------------------------------------------------------------------------
 * WASM native export API
 *
 * Draw calls no longer flush immediately.  The WASM app is expected to call
 * display_flush() when it has finished composing a frame.  As a safety net
 * g_auto_flush_work fires 50 ms after the last draw call.
 * -------------------------------------------------------------------------*/

#ifdef CONFIG_AKIRA_WASM_RUNTIME

static void auto_flush_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    akira_display_flush();
}

static K_WORK_DELAYABLE_DEFINE(g_auto_flush_work, auto_flush_handler);

static inline void schedule_auto_flush(void)
{
    k_work_reschedule(&g_auto_flush_work, K_MSEC(50));
}

int akira_native_display_rect(wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_rect(x, y, w, h, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_text(wasm_exec_env_t exec_env, int32_t x, int32_t y, const char *text, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_text(x, y, text, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_text_large(wasm_exec_env_t exec_env, int x, int y, const char *text, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_text_large(x, y, text, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_clear(wasm_exec_env_t exec_env, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_clear((uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_pixel(wasm_exec_env_t exec_env, int32_t x, int32_t y, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_pixel(x, y, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_flush(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    /* Cancel pending auto-flush — we're flushing explicitly right now */
    k_work_cancel_delayable(&g_auto_flush_work);
    akira_display_flush();
    return 0;
}

int akira_native_display_get_size(wasm_exec_env_t exec_env, int32_t *w_out, int32_t *h_out)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    if (!w_out || !h_out) {
        return -EINVAL;
    }

    int w = 0, h = 0;
    akira_display_get_size(&w, &h);
    *w_out = (int32_t)w;
    *h_out = (int32_t)h;
    return 0;
}

/* Phase 3.5 native wrappers */

int akira_native_display_line(wasm_exec_env_t exec_env,
    int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_line(x0, y0, x1, y1, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_circle(wasm_exec_env_t exec_env,
    int32_t cx, int32_t cy, int32_t r, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_circle(cx, cy, r, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_circle_fill(wasm_exec_env_t exec_env,
    int32_t cx, int32_t cy, int32_t r, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_circle_fill(cx, cy, r, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_triangle(wasm_exec_env_t exec_env,
    int32_t x0, int32_t y0, int32_t x1, int32_t y1,
    int32_t x2, int32_t y2, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_triangle(x0, y0, x1, y1, x2, y2, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_triangle_fill(wasm_exec_env_t exec_env,
    int32_t x0, int32_t y0, int32_t x1, int32_t y1,
    int32_t x2, int32_t y2, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_triangle_fill(x0, y0, x1, y1, x2, y2, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_rect_outline(wasm_exec_env_t exec_env,
    int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_rect_outline(x, y, w, h, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_bitmap(wasm_exec_env_t exec_env,
    int32_t x, int32_t y, int32_t w, int32_t h,
    const uint8_t *data, uint32_t data_size)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    if (!data) return -EINVAL;
    if ((int64_t)data_size < (int64_t)w * h * 2) {
        LOG_ERR("display_bitmap: data_size %u < w*h*2 (%d)", data_size, w * h * 2);
        return -EINVAL;
    }
    akira_display_bitmap(x, y, w, h, (const uint16_t *)data);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_raw_write(wasm_exec_env_t exec_env,
    int32_t x, int32_t y, int32_t w, int32_t h,
    const uint8_t *data, uint32_t data_size)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    if (!data) return -EINVAL;
    if ((int64_t)data_size < (int64_t)w * h * 2) {
        LOG_ERR("display_raw_write: data_size %u < w*h*2 (%d)", data_size, w * h * 2);
        return -EINVAL;
    }
    /* Write packed pixel buffer directly to display hardware, bypassing the
     * OS framebuffer.  pitch = w (no stride) — lets the display controller
     * receive exactly w*h pixels with a single SPI window transaction.   */
    return akira_display_hal_write_raw(x, y, w, h, (const uint16_t *)data);
}

int akira_native_display_bitmap_transparent(wasm_exec_env_t exec_env,
    int32_t x, int32_t y, int32_t w, int32_t h,
    const uint8_t *data, uint32_t data_size, uint32_t key)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    if (!data) return -EINVAL;
    if ((int64_t)data_size < (int64_t)w * h * 2) {
        LOG_ERR("display_bitmap_transparent: data_size %u < w*h*2 (%d)", data_size, w * h * 2);
        return -EINVAL;
    }
    akira_display_bitmap_transparent(x, y, w, h, (const uint16_t *)data, (uint16_t)key);
    schedule_auto_flush();
    return 0;
}

/* Phase 4 native wrappers */

int akira_native_display_hline(wasm_exec_env_t exec_env,
    int32_t x, int32_t y, int32_t len, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_hline(x, y, len, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_vline(wasm_exec_env_t exec_env,
    int32_t x, int32_t y, int32_t len, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_vline(x, y, len, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_number(wasm_exec_env_t exec_env,
    int32_t x, int32_t y, int32_t value, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_number(x, y, value, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_progress_bar(wasm_exec_env_t exec_env,
    int32_t x, int32_t y, int32_t w, int32_t h,
    int32_t value, int32_t max_val, uint32_t fg, uint32_t bg)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_progress_bar(x, y, w, h, value, max_val,
                               (uint16_t)fg, (uint16_t)bg);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_rounded_rect(wasm_exec_env_t exec_env,
    int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_rounded_rect(x, y, w, h, radius, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

int akira_native_display_rounded_rect_fill(wasm_exec_env_t exec_env,
    int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(-EBUSY);
    akira_display_rounded_rect_fill(x, y, w, h, radius, (uint16_t)color);
    schedule_auto_flush();
    return 0;
}

#endif