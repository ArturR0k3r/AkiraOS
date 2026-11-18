/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
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
 *
 * Containment (see include/akira_app_containment.h): any argument values from
 * an app are safe.  Geometry is clipped in 64-bit arithmetic and every loop is
 * bounded by the panel; pixel buffers are length- and alignment-checked; the
 * calls that hold the panel's bus lock run as native critical sections; and
 * the auto-flush an app arms is disarmed when that app ends.
 *
 * AkiraConsole additions: the OS shell can hold the display (apps then get
 * -EBUSY, see console_shell/display_ownership.h); flushes hand the
 * framebuffer to the display compositor (akira_framebuffer_present*(), double
 * buffered on boards with a PSRAM framebuffer); a 16x28 "huge" font; and an
 * asynchronous raw write whose transfer the app's release hook finishes or
 * drops before the app's memory is freed.
 */
#include "akira_api.h"
#include "display_clip.h"
#include <akira_app_containment.h>
#include <runtime/security.h>
#include <drivers/platform_hal.h>
#include "../drivers/display/fonts.h"
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <stdlib.h> /* abs() */
#include <string.h> /* memcpy() */

#if defined(CONFIG_AKIRA_OS_SHELL)
#include <console_shell/display_ownership.h>
#endif

/* ------------------------------------------------------------------ */
/* Display ownership                                                     */
/*                                                                       */
/* The framebuffer is a single shared surface with no compositor, so two  */
/* apps drawing at once produce interleaved garbage and let a hostile app */
/* paint over another app's UI (spoofing a PIN prompt, say).  This used   */
/* to be a `do {} while (0)` stub, i.e. no arbitration at all: every app  */
/* holding display.write could draw at any time.                          */
/*                                                                       */
/* Policy: first WASM app to draw claims the display; others get -EBUSY   */
/* until it exits.  Ownership is released explicitly when the instance is */
/* torn down (see akira_display_release_owner callers in akira_runtime.c) */
/* and, as a safety net, stolen from an owner the runtime no longer knows */
/* about — otherwise an app that died on a path we failed to hook would   */
/* lock the display until reboot.                                         */
/*                                                                       */
/* exec_env == NULL means a trusted first-party/native caller (the OS     */
/* shell, boot animation, wait screen).  Those bypass the check, matching */
/* akira_security_check_native().                                         */
/*                                                                       */
/* With the native console shell (CONFIG_AKIRA_OS_SHELL) there is a       */
/* second level above this: the shell owns the display at boot, hands it  */
/* to the running app (akira_display_release_to_wasm()) and takes it back */
/* for HOME or a Seal prompt.  While the shell holds it every app draw    */
/* call gets -EBUSY, and the app retries on its next frame.               */
/* ------------------------------------------------------------------ */
#if defined(CONFIG_AKIRA_WASM_RUNTIME)

/* Defined in runtime/akira_runtime.c; declared extern here rather than in
 * akira_runtime.h to match how akira_memory_api.c already reaches it. */
extern int get_slot_for_module_inst(wasm_module_inst_t inst);

static wasm_module_inst_t g_display_owner; /* NULL = unowned */
static struct k_spinlock g_display_owner_lock;

/* True unless the OS shell holds the display. */
static inline bool display_owner_is_wasm(void)
{
#if defined(CONFIG_AKIRA_OS_SHELL)
    return akira_display_get_owner() == DISPLAY_OWNER_WASM;
#else
    return true;
#endif
}

static bool display_owner_acquire(wasm_exec_env_t exec_env)
{
    if (!exec_env)
        return true; /* trusted native caller */

    /* The shell has the display: no app may draw, nor claim it. */
    if (!display_owner_is_wasm())
        return false;

    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst)
        return false;

    k_spinlock_key_t key = k_spin_lock(&g_display_owner_lock);
    bool ok;

    if (g_display_owner == inst || g_display_owner == NULL)
    {
        g_display_owner = inst;
        ok = true;
    }
    else if (get_slot_for_module_inst(g_display_owner) < 0)
    {
        /* Recorded owner is not a live instance any more — reclaim.  Pointer
         * comparison only (hash-map lookup), never a dereference, so a stale
         * pointer here is safe to test. */
        g_display_owner = inst;
        ok = true;
    }
    else
    {
        ok = false;
    }

    k_spin_unlock(&g_display_owner_lock, key);
    return ok;
}

void akira_display_release_owner(wasm_module_inst_t inst)
{
    if (!inst)
        return;

    k_spinlock_key_t key = k_spin_lock(&g_display_owner_lock);
    if (g_display_owner == inst)
        g_display_owner = NULL;
    k_spin_unlock(&g_display_owner_lock, key);
}

#define AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, ret_val) \
    do                                                        \
    {                                                         \
        if (!display_owner_acquire(exec_env))                 \
        {                                                     \
            return (ret_val);                                 \
        }                                                     \
    } while (0)

#else /* no WASM runtime — nothing can contend for the display */

void akira_display_release_owner(wasm_module_inst_t inst) { (void)inst; }

#define AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, ret_val) \
    do                                                         \
    {                                                          \
        (void)(exec_env);                                      \
    } while (0)

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/* ---------------------------------------------------------------------------
 * Draw target and clipping helpers
 *
 * Every primitive below is reachable from WASM with arbitrary 32-bit
 * arguments, so all geometry is computed in 64 bits and clipped to the panel
 * (display_clip.h) before anything is iterated or written: no int overflow,
 * no negative span, and no loop longer than the panel, DISP_RADIUS_MAX for
 * arcs or DISP_TEXT_MAX_BYTES for text.
 * -------------------------------------------------------------------------*/

/* Arcs (circles, rounded corners) iterate O(radius): bound it.  4096 is ten
 * panels wide, so the clamp only changes shapes that are off-screen anyway. */
#define DISP_RADIUS_MAX 4096

/* Text walks at most this many bytes per call.  Drawing is bounded by the
 * panel already; this bounds the scan over a string that may span the app's
 * whole linear memory. */
#define DISP_TEXT_MAX_BYTES 4096

static bool panel_size(int *w, int *h)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    *w = 240;
    *h = 320;
    return true;
#else
    struct display_capabilities caps;

    if (akira_display_hal_get_capabilities(&caps) < 0)
        return false;
    *w = caps.x_resolution;
    *h = caps.y_resolution;
    return *w > 0 && *h > 0;
#endif
}

#if !AKIRA_PLATFORM_NATIVE_SIM
#define DISP_FB_MAX_PIXELS AKIRA_FB_MAX_PIXELS

/* The framebuffer to draw into, or NULL if there is none or the panel does
 * not fit in it (row offsets would then run past the array).  Whether there
 * is a usable one is decided once: on boards without one the per-pixel paths
 * would otherwise ask (and log) on every pixel, a flood any app can trigger.
 * The pointer itself is re-read on every call: with the double-buffered
 * framebuffer (AkiraConsole) akira_framebuffer_get() returns the buffer being
 * drawn, which flips on every present. */
#define DISP_FB_UNPROBED 0
#define DISP_FB_USABLE   1
#define DISP_FB_NONE     2

static uint16_t *display_fb(int pw, int ph)
{
    static atomic_t state;
    atomic_val_t st = atomic_get(&state);

    if (st == DISP_FB_UNPROBED)
    {
        uint16_t *p = akira_framebuffer_get();

        if (p && (int64_t)pw * ph > DISP_FB_MAX_PIXELS)
        {
            LOG_ERR("Panel %dx%d does not fit the %d-pixel framebuffer; drawing disabled",
                    pw, ph, DISP_FB_MAX_PIXELS);
            p = NULL;
        }
        st = p ? DISP_FB_USABLE : DISP_FB_NONE;
        atomic_set(&state, st);
    }
    return (st == DISP_FB_USABLE) ? akira_framebuffer_get() : NULL;
}
#endif

/* Panel size and the framebuffer to draw into; false = nothing to draw on.
 * On native_sim *fb is NULL and pixels go through akira_sim_draw_pixel(). */
static bool draw_target(uint16_t **fb, int *pw, int *ph)
{
    if (!panel_size(pw, ph))
        return false;
#if AKIRA_PLATFORM_NATIVE_SIM
    *fb = NULL;
    return true;
#else
    *fb = display_fb(*pw, *ph);
    return *fb != NULL;
#endif
}

#if !AKIRA_PLATFORM_NATIVE_SIM
/* Fill @p n pixels from @p dst, two per 32-bit store.  A 32-bit store needs a
 * 4-byte aligned address on cores without unaligned access (Cortex-M STRD/STM,
 * RISC-V), so a row starting on an odd pixel writes that pixel first. */
static void fill_span(uint16_t *dst, size_t n, uint16_t color)
{
    if (n > 0 && ((uintptr_t)dst & 2U))
    {
        *dst++ = color;
        n--;
    }
    uint32_t c32 = ((uint32_t)color << 16) | color;
    uint32_t *p = (uint32_t *)(void *)dst;
    for (size_t i = 0; i < n / 2; i++)
        p[i] = c32;
    if (n & 1)
        dst[n - 1] = color;
}
#endif

/* Fill [x0, x1) x [y0, y1), clipped to the panel.  64-bit edges, so callers
 * pass derived geometry (x + w, x + w - r, ...) without overflowing int. */
static void fill_box64(int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint16_t color)
{
    uint16_t *fb;
    int pw, ph, cx0, cy0, cx1, cy1;

    if (!draw_target(&fb, &pw, &ph) ||
        !akira_display_clip_box(x0, y0, x1, y1, pw, ph, &cx0, &cy0, &cx1, &cy1))
        return;

#if AKIRA_PLATFORM_NATIVE_SIM
    for (int py = cy0; py < cy1; py++)
        for (int px = cx0; px < cx1; px++)
            akira_sim_draw_pixel(px, py, color);
#else
    /* Fill the first row, then memcpy it to the remaining rows: the optimised
     * memcpy beats a scalar per-pixel store loop. */
    const size_t span = (size_t)(cx1 - cx0);
    uint16_t *row0 = fb + (size_t)cy0 * (size_t)pw + (size_t)cx0;

    fill_span(row0, span, color);
    for (int py = cy0 + 1; py < cy1; py++)
        memcpy(fb + (size_t)py * (size_t)pw + (size_t)cx0, row0, span * sizeof(uint16_t));
#endif
}

/* One pixel at 64-bit coordinates; anything off the panel is dropped. */
static inline void plot64(int64_t x, int64_t y, uint16_t color)
{
    if (x < 0 || y < 0 || x > INT16_MAX || y > INT16_MAX)
        return;
    akira_display_pixel((int)x, (int)y, color);
}

void akira_display_clear(uint16_t color)
{
    uint16_t *fb;
    int pw, ph;

    if (!draw_target(&fb, &pw, &ph))
        return;

#if AKIRA_PLATFORM_NATIVE_SIM
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++)
            akira_sim_draw_pixel(x, y, color);
#else
    /* draw_target() checked that pw * ph fits the framebuffer. */
    fill_span(fb, (size_t)pw * (size_t)ph, color);
#endif
}

void akira_display_pixel(int x, int y, uint16_t color)
{
    uint16_t *fb;
    int pw, ph;

    if (!draw_target(&fb, &pw, &ph))
        return;
    if (x < 0 || x >= pw || y < 0 || y >= ph)
        return;

#if AKIRA_PLATFORM_NATIVE_SIM
    akira_sim_draw_pixel(x, y, color);
#else
    fb[(size_t)y * (size_t)pw + (size_t)x] = color;
#endif
}

void akira_display_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0)
        return;
    fill_box64(x, y, (int64_t)x + w, (int64_t)y + h, color);
}

/* draw_string() with bounded work: at most DISP_TEXT_MAX_BYTES bytes are
 * read, glyphs off the panel are skipped without drawing, and the walk stops
 * once the cursor is below the panel (it only moves down).  The cursor is
 * 64-bit, so no position or string length can overflow it.  Layout ('\n',
 * '\t', spacing) matches draw_string(). */
static void text_draw(int x, int y, const char *text, uint16_t color, FontType font)
{
    int pw, ph;

    if (!text || !panel_size(&pw, &ph))
        return;

    const int cw = (font == FONT_7X10)   ? FONT7X10_WIDTH
                   : (font == FONT_HERO) ? FONT16X28_WIDTH
                                         : FONT11X18_WIDTH;
    const int ch = (font == FONT_7X10)   ? FONT7X10_HEIGHT
                   : (font == FONT_HERO) ? FONT16X28_HEIGHT
                                         : FONT11X18_HEIGHT;
    const int adv = cw + 1; /* 1 pixel between characters */
    const int tab_size = 4;
    int64_t cx = x, cy = y;

    for (size_t i = 0; i < DISP_TEXT_MAX_BYTES && text[i] != '\0'; i++)
    {
        if (cy >= ph)
            break;

        char c = text[i];

        if (c == '\n')
        {
            cx = x;
            cy += ch + 2; /* 2 pixels line spacing */
        }
        else if (c == '\t')
        {
            int64_t col = (cx - x) / adv;
            cx += (tab_size - col % tab_size) * adv;
        }
        else
        {
            /* Visible glyph: its box [cx, cx + cw) x [cy, cy + ch) meets the
             * panel, so both coordinates fit int and draw_char's x + col,
             * y + row cannot overflow. */
            if (cx + cw > 0 && cx < pw && cy + ch > 0)
                draw_char((int)cx, (int)cy, c, color, akira_display_pixel, font);
            cx += adv;
        }
    }
}

void akira_display_text(int x, int y, const char *text, uint16_t color)
{
    text_draw(x, y, text, color, FONT_7X10);
}

void akira_display_text_large(int x, int y, const char *text, uint16_t color)
{
    text_draw(x, y, text, color, FONT_11X18);
}

/* 16x28 "hero" font: the one big value per screen (clock, percentage). */
void akira_display_text_huge(int x, int y, const char *text, uint16_t color)
{
    text_draw(x, y, text, color, FONT_HERO);
}

#if !AKIRA_PLATFORM_NATIVE_SIM
/* Without a panel or a framebuffer there is nothing to present, and the HAL
 * would log an error on every call. */
static bool can_present(void)
{
    uint16_t *fb;
    int pw, ph;

    return draw_target(&fb, &pw, &ph);
}
#endif

/* Flushes hand the framebuffer just drawn to the display compositor thread
 * (drivers/display/display_hal.c), which blits it while the next frame is
 * drawn into the other buffer; a present waits only while the compositor is
 * still busy with the previous frame.  Without a framebuffer there is
 * nothing to present. */
void akira_display_flush(void)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    akira_sim_show_display();
#else
    if (!can_present())
        return;
    akira_framebuffer_present();
#endif
}

void akira_display_flush_rows(int y0, int y1)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    ARG_UNUSED(y0);
    ARG_UNUSED(y1);
    akira_sim_show_display();
#else
    if (!can_present())
        return;
    akira_framebuffer_present_rows(y0, y1);
#endif
}

void akira_display_flush_rect(int x, int y, int w, int h)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    ARG_UNUSED(x);
    ARG_UNUSED(y);
    ARG_UNUSED(w);
    ARG_UNUSED(h);
    akira_sim_show_display();
#else
    if (!can_present())
        return;
    akira_framebuffer_present_rect(x, y, w, h);
#endif
}

void akira_display_get_size(int *width, int *height)
{
    int w, h;

    if (!panel_size(&w, &h))
    {
        w = 0;
        h = 0;
    }
    if (width)
        *width = w;
    if (height)
        *height = h;
}

/* ---------------------------------------------------------------------------
 * Phase 3.5 — Additional primitives
 * -------------------------------------------------------------------------*/

void akira_display_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int pw, ph, ax, ay, bx, by;

    /* Clip first: Bresenham then runs at most max(panel w, h) + 1 steps,
     * where the unclipped segment could be billions. */
    if (!panel_size(&pw, &ph) ||
        !akira_display_clip_line(x0, y0, x1, y1, pw, ph, &ax, &ay, &bx, &by))
        return;

    /* Bresenham's line algorithm — integer arithmetic only, no FPU */
    int dx = abs(bx - ax);
    int dy = -abs(by - ay);
    int sx = (ax < bx) ? 1 : -1;
    int sy = (ay < by) ? 1 : -1;
    int err = dx + dy;

    while (1)
    {
        akira_display_pixel(ax, ay, color);
        if (ax == bx && ay == by)
            break;
        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            ax += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            ay += sy;
        }
    }
}

/* Bound the radius and reject circles whose bounding box misses the panel.
 * On success the centre is within DISP_RADIUS_MAX of the panel, so int math
 * on cx +- r cannot overflow. */
static bool circle_prepare(int cx, int cy, int *r)
{
    int pw, ph;

    if (*r <= 0 || !panel_size(&pw, &ph))
        return false;
    if (*r > DISP_RADIUS_MAX)
        *r = DISP_RADIUS_MAX;
    if ((int64_t)cx + *r < 0 || (int64_t)cx - *r >= pw ||
        (int64_t)cy + *r < 0 || (int64_t)cy - *r >= ph)
        return false;
    return true;
}

void akira_display_circle(int cx, int cy, int r, uint16_t color)
{
    /* Midpoint circle algorithm — O(r) iterations, r <= DISP_RADIUS_MAX */
    if (!circle_prepare(cx, cy, &r))
        return;
    int x = r, y = 0, d = 1 - r;

    while (x >= y)
    {
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
    if (!circle_prepare(cx, cy, &r))
        return;
    int x = r, y = 0, d = 1 - r;

    while (x >= y)
    {
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
    int pw, ph;

    if (!panel_size(&pw, &ph))
        return;

    /* Clamped vertices keep every product below inside int64. */
    int64_t ax = akira_display_clamp_vertex(x0), ay = akira_display_clamp_vertex(y0);
    int64_t bx = akira_display_clamp_vertex(x1), by = akira_display_clamp_vertex(y1);
    int64_t cx = akira_display_clamp_vertex(x2), cy = akira_display_clamp_vertex(y2);

    /* Sort vertices ascending by Y */
#define SWAP_I64(a, b)    \
    do                    \
    {                     \
        int64_t _t = (a); \
        (a) = (b);        \
        (b) = _t;         \
    } while (0)
    if (ay > by)
    {
        SWAP_I64(ax, bx);
        SWAP_I64(ay, by);
    }
    if (by > cy)
    {
        SWAP_I64(bx, cx);
        SWAP_I64(by, cy);
    }
    if (ay > by)
    {
        SWAP_I64(ax, bx);
        SWAP_I64(ay, by);
    }
#undef SWAP_I64

    /* Scanline rasteriser over the visible rows only (<= panel height). */
    if (cy < 0 || ay >= ph)
        return;
    int64_t y_first = MAX(ay, 0);
    int64_t y_last = MIN(cy, (int64_t)ph - 1);

    for (int64_t y = y_first; y <= y_last; y++)
    {
        int64_t xa = (cy != ay) ? ax + (cx - ax) * (y - ay) / (cy - ay) : ax;
        int64_t xb;
        if (y <= by)
            xb = (by != ay) ? ax + (bx - ax) * (y - ay) / (by - ay) : bx;
        else
            xb = (cy != by) ? bx + (cx - bx) * (y - by) / (cy - by) : bx;

        if (xa > xb)
        {
            int64_t t = xa;
            xa = xb;
            xb = t;
        }
        fill_box64(xa, y, xb + 1, y + 1, color);
    }
}

void akira_display_rect_outline(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0)
        return;
    int64_t x0 = x, y0 = y, x1 = (int64_t)x + w, y1 = (int64_t)y + h;

    fill_box64(x0, y0, x1, y0 + 1, color);     /* top    */
    fill_box64(x0, y1 - 1, x1, y1, color);     /* bottom */
    fill_box64(x0, y0, x0 + 1, y1, color);     /* left   */
    fill_box64(x1 - 1, y0, x1, y1, color);     /* right  */
}

/* ---------------------------------------------------------------------------
 * Phase 4 — UI helper primitives
 * -------------------------------------------------------------------------*/

void akira_display_hline(int x, int y, int len, uint16_t color)
{
    /* Horizontal pixel run — thin wrapper over rect for clarity */
    if (len <= 0)
        return;
    akira_display_rect(x, y, len, 1, color);
}

void akira_display_vline(int x, int y, int len, uint16_t color)
{
    /* Vertical pixel run — a 1-pixel-wide rect */
    if (len <= 0)
        return;
    akira_display_rect(x, y, 1, len, color);
}

void akira_display_number(int x, int y, int32_t value, uint16_t color)
{
    /* Convert integer to decimal string, then render with the small font.
     * Buffer: sign + 10 digits + NUL → 12 bytes is enough for int32_t. */
    char buf[13];
    int i = 12;
    buf[i] = '\0';

    int32_t v = value;
    int negative = (v < 0);
    if (v == 0)
    {
        buf[--i] = '0';
    }
    else
    {
        /* Work with positive magnitude; handle INT32_MIN edge case */
        uint32_t uv = negative ? (uint32_t)(-(v + 1)) + 1U : (uint32_t)v;
        while (uv)
        {
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
    if (w <= 0 || h <= 0 || max_val <= 0)
        return;

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

/* Common set-up for rounded rects: bounded radius, 64-bit right/bottom edges
 * (exclusive), and an early out when the box misses the panel.
 * false = nothing to draw. */
static bool rounded_prepare(int x, int y, int w, int h, int *radius,
                            int64_t *x1, int64_t *y1)
{
    int pw, ph;

    if (w <= 0 || h <= 0 || !panel_size(&pw, &ph))
        return false;
    *x1 = (int64_t)x + w;
    *y1 = (int64_t)y + h;
    if (*x1 <= 0 || x >= pw || *y1 <= 0 || y >= ph)
        return false;

    /* Clamp radius so the arcs fit, and so the arc loops stay short. */
    int max_r = MIN(MIN(w, h) / 2, DISP_RADIUS_MAX);
    *radius = CLAMP(*radius, 0, max_r);
    return true;
}

void akira_display_rounded_rect(int x, int y, int w, int h,
                                int radius, uint16_t color)
{
    int64_t x1, y1;

    if (!rounded_prepare(x, y, w, h, &radius, &x1, &y1))
        return;
    if (radius == 0)
    {
        akira_display_rect_outline(x, y, w, h, color);
        return;
    }

    int64_t r = radius;

    /* Four straight edge segments */
    fill_box64(x + r, y, x1 - r, y + 1, color);      /* top    */
    fill_box64(x + r, y1 - 1, x1 - r, y1, color);    /* bottom */
    fill_box64(x, y + r, x + 1, y1 - r, color);      /* left   */
    fill_box64(x1 - 1, y + r, x1, y1 - r, color);    /* right  */

    /* Four corner arcs via midpoint algorithm, each restricted to its quadrant.
     * Corner centres: TL=(x+r, y+r), TR=(x+w-1-r, y+r),
     *                 BL=(x+r, y+h-1-r), BR=(x+w-1-r, y+h-1-r) */
    int64_t cx_l = x + r;
    int64_t cx_r = x1 - 1 - r;
    int64_t cy_t = y + r;
    int64_t cy_b = y1 - 1 - r;

    int px = radius, py = 0, d = 1 - radius;
    while (px >= py)
    {
        /* TL */ plot64(cx_l - px, cy_t - py, color);
        plot64(cx_l - py, cy_t - px, color);
        /* TR */ plot64(cx_r + px, cy_t - py, color);
        plot64(cx_r + py, cy_t - px, color);
        /* BL */ plot64(cx_l - px, cy_b + py, color);
        plot64(cx_l - py, cy_b + px, color);
        /* BR */ plot64(cx_r + px, cy_b + py, color);
        plot64(cx_r + py, cy_b + px, color);
        py++;
        d += (d < 0) ? 2 * py + 1 : (px--, 2 * (py - px) + 1);
    }
}

void akira_display_rounded_rect_fill(int x, int y, int w, int h,
                                     int radius, uint16_t color)
{
    int64_t x1, y1;

    if (!rounded_prepare(x, y, w, h, &radius, &x1, &y1))
        return;
    if (radius == 0)
    {
        akira_display_rect(x, y, w, h, color);
        return;
    }

    int64_t r = radius;

    /* Central band between the rounded caps */
    fill_box64(x, y + r, x1, y1 - r, color);

    /* Filled corner quarter-discs using midpoint scanline fill */
    int64_t cx_l = x + r;
    int64_t cx_r = x1 - 1 - r;
    int64_t cy_t = y + r;
    int64_t cy_b = y1 - 1 - r;

    int px = radius, py = 0, d = 1 - radius;
    while (px >= py)
    {
        /* Fill horizontal spans for top and bottom rounded caps */
        fill_box64(cx_l - px, cy_t - py, cx_r + px + 1, cy_t - py + 1, color);
        fill_box64(cx_l - px, cy_b + py, cx_r + px + 1, cy_b + py + 1, color);
        if (py != 0)
        {
            fill_box64(cx_l - py, cy_t - px, cx_r + py + 1, cy_t - px + 1, color);
            fill_box64(cx_l - py, cy_b + px, cx_r + py + 1, cy_b + px + 1, color);
        }
        py++;
        d += (d < 0) ? 2 * py + 1 : (px--, 2 * (py - px) + 1);
    }
}

/* Offset of image pixel (px, py), given in panel coordinates, in a w-wide
 * image placed at (x, y).  Only called for pixels inside the clipped image,
 * so the offsets lie in [0, w) x [0, h) and the result indexes the w * h
 * pixels the caller supplied.  64-bit differences: py - y overflows int for
 * y near INT32_MIN. */
static inline size_t image_index(int px, int py, int x, int y, int w)
{
    return (size_t)((int64_t)py - y) * (size_t)w + (size_t)((int64_t)px - x);
}

void akira_display_bitmap(int x, int y, int w, int h, const uint16_t *data)
{
    uint16_t *fb;
    int pw, ph, x1, y1, x2, y2;

    if (!data || !draw_target(&fb, &pw, &ph) ||
        !akira_display_clip_rect(x, y, w, h, pw, ph, &x1, &y1, &x2, &y2))
        return;

#if AKIRA_PLATFORM_NATIVE_SIM
    for (int py = y1; py < y2; py++)
        for (int px = x1; px < x2; px++)
            akira_sim_draw_pixel(px, py, data[image_index(px, py, x, y, w)]);
#else
    /* akira_display_clip_rect() guarantees x2 > x1, so span >= 1: a fully
     * clipped rect can no longer turn into a negative-span memcpy. */
    const size_t span = (size_t)(x2 - x1);

    for (int py = y1; py < y2; py++)
    {
        const uint16_t *src = data + image_index(x1, py, x, y, w);
        uint16_t *dst = fb + (size_t)py * (size_t)pw + (size_t)x1;
        memcpy(dst, src, span * sizeof(uint16_t));
    }
#endif
}

void akira_display_bitmap_transparent(int x, int y, int w, int h,
                                      const uint16_t *data, uint16_t key)
{
    uint16_t *fb;
    int pw, ph, x1, y1, x2, y2;

    if (!data || !draw_target(&fb, &pw, &ph) ||
        !akira_display_clip_rect(x, y, w, h, pw, ph, &x1, &y1, &x2, &y2))
        return;

#if AKIRA_PLATFORM_NATIVE_SIM
    for (int py = y1; py < y2; py++)
        for (int px = x1; px < x2; px++)
        {
            uint16_t c = data[image_index(px, py, x, y, w)];
            if (c != key)
                akira_sim_draw_pixel(px, py, c);
        }
#else
    for (int py = y1; py < y2; py++)
    {
        const uint16_t *src = data + image_index(x1, py, x, y, w);
        uint16_t *dst = fb + (size_t)py * (size_t)pw;
        for (int px = x1; px < x2; px++)
        {
            uint16_t c = src[px - x1];
            if (c != key)
                dst[px] = c;
        }
    }
#endif
}

/* ---------------------------------------------------------------------------
 * WASM native export API
 *
 * Draw calls do not flush immediately.  The WASM app is expected to call
 * display_flush() when it has finished composing a frame.  As a safety net
 * g_auto_flush_work fires 50 ms after the last draw call.
 *
 * Every primitive above clips in 64-bit arithmetic and bounds its loops, so
 * any coordinate values are safe here; the wrappers validate buffers.
 * -------------------------------------------------------------------------*/

#ifdef CONFIG_AKIRA_WASM_RUNTIME

/* Slot of the app whose draw call armed the auto-flush (-1 = none). */
static atomic_t g_auto_flush_slot = ATOMIC_INIT(-1);

static void auto_flush_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    /* Disarmed by the app's release: the framebuffer is no longer its. */
    if (atomic_get(&g_auto_flush_slot) < 0)
        return;
    /* Armed by an app that has since lost the display to the shell: the
     * framebuffer is the shell's now, and presenting it half-drawn would
     * flash garbage. */
    if (!display_owner_is_wasm())
        return;
    akira_display_flush();
}

static K_WORK_DELAYABLE_DEFINE(g_auto_flush_work, auto_flush_handler);

static inline void schedule_auto_flush(wasm_exec_env_t exec_env)
{
    atomic_set(&g_auto_flush_slot, akira_runtime_slot_of_exec_env(exec_env));
    k_work_reschedule(&g_auto_flush_work, K_MSEC(50));
}

/* A pixel buffer from WASM memory.  WAMR's "*~" signature validated
 * [data, data + data_size); also require bytes for the whole w x h image
 * (64-bit: w * h * 2 overflows int) and 2-byte alignment, since the
 * primitives read it as uint16_t and an odd address faults on cores without
 * unaligned access (Cortex-M0, RISC-V).  w or h <= 0 passes and draws
 * nothing, as before. */
static int check_pixels(const uint8_t *data, uint32_t data_size, int32_t w, int32_t h)
{
    if (!data || ((uintptr_t)data & 1U))
        return -EINVAL;
    if ((int64_t)data_size < (int64_t)w * h * 2)
    {
        LOG_WRN_RATELIMIT("display: pixel buffer of %u B is smaller than %d x %d x 2",
                          data_size, w, h);
        return -EINVAL;
    }
    return 0;
}

int akira_native_display_rect(wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_rect(x, y, w, h, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

/* Text natives: "$" makes WAMR check the string is NUL-terminated inside the
 * app's memory; text_draw() then reads at most DISP_TEXT_MAX_BYTES of it. */
int akira_native_display_text(wasm_exec_env_t exec_env, int32_t x, int32_t y, const char *text, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_text(x, y, text, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_text_large(wasm_exec_env_t exec_env, int x, int y, const char *text, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_text_large(x, y, text, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_text_huge(wasm_exec_env_t exec_env, int x, int y, const char *text, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_text_huge(x, y, text, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_clear(wasm_exec_env_t exec_env, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_clear((uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_pixel(wasm_exec_env_t exec_env, int32_t x, int32_t y, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_pixel(x, y, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_flush(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    /* Cancel pending auto-flush — we're flushing explicitly right now */
    k_work_cancel_delayable(&g_auto_flush_work);
    /* display_write() holds the panel driver's bus locks for one frame
     * (bounded: one panel's worth of SPI).  An abort inside it would leave
     * them held, and every later flush, including the auto-flush on the
     * system workqueue, would block on them forever. */
    akira_native_critical_enter(exec_env);
    akira_display_flush();
    akira_native_critical_exit(exec_env);
    return 0;
}

int akira_native_display_get_size(wasm_exec_env_t exec_env, int32_t *w_out, int32_t *h_out)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    if (!w_out || !h_out)
    {
        return -EINVAL;
    }

    /* "(**)i" only validates one byte per pointer; check the full int32_t. */
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!inst ||
        !wasm_runtime_validate_native_addr(inst, w_out, sizeof(*w_out)) ||
        !wasm_runtime_validate_native_addr(inst, h_out, sizeof(*h_out)))
    {
        return -EFAULT;
    }

    int w = 0, h = 0;
    akira_display_get_size(&w, &h);
    /* memcpy: an app pointer need not be 4-byte aligned. */
    int32_t w32 = w, h32 = h;
    memcpy(w_out, &w32, sizeof(w32));
    memcpy(h_out, &h32, sizeof(h32));
    return 0;
}

/* Pointer-free size queries (AkiraConsole SDK): 0 without display.write or
 * without a panel, so an app can't mistake an error for a size. */
int akira_native_display_get_width(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, 0);
    int w = 0, h = 0;
    akira_display_get_size(&w, &h);
    return w;
}

int akira_native_display_get_height(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, 0);
    int w = 0, h = 0;
    akira_display_get_size(&w, &h);
    return h;
}

/* Phase 3.5 native wrappers */

int akira_native_display_line(wasm_exec_env_t exec_env,
                              int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_line(x0, y0, x1, y1, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_circle(wasm_exec_env_t exec_env,
                                int32_t cx, int32_t cy, int32_t r, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_circle(cx, cy, r, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_circle_fill(wasm_exec_env_t exec_env,
                                     int32_t cx, int32_t cy, int32_t r, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_circle_fill(cx, cy, r, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_triangle(wasm_exec_env_t exec_env,
                                  int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                  int32_t x2, int32_t y2, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_triangle(x0, y0, x1, y1, x2, y2, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_triangle_fill(wasm_exec_env_t exec_env,
                                       int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                       int32_t x2, int32_t y2, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_triangle_fill(x0, y0, x1, y1, x2, y2, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_rect_outline(wasm_exec_env_t exec_env,
                                      int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_rect_outline(x, y, w, h, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_bitmap(wasm_exec_env_t exec_env,
                                int32_t x, int32_t y, int32_t w, int32_t h,
                                const uint8_t *data, uint32_t data_size)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    int ret = check_pixels(data, data_size, w, h);
    if (ret < 0)
        return ret;
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_bitmap(x, y, w, h, (const uint16_t *)data);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_raw_write(wasm_exec_env_t exec_env,
                                   int32_t x, int32_t y, int32_t w, int32_t h,
                                   const uint8_t *data, uint32_t data_size)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    int ret = check_pixels(data, data_size, w, h);
    if (ret < 0)
        return ret;
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    /* Write packed pixel buffer directly to display hardware, bypassing the
     * OS framebuffer.  pitch = w (no stride) — lets the display controller
     * receive exactly w*h pixels with a single SPI window transaction.  The
     * HAL rejects any window that is not entirely on the panel, so one call
     * moves at most a panel's worth while holding the bus: bounded, and an
     * abort inside it would leak the bus lock. */
    akira_native_critical_enter(exec_env);
    ret = akira_display_hal_write_raw(x, y, w, h, (const uint16_t *)data);
    akira_native_critical_exit(exec_env);
    return ret;
}

/* ---------------------------------------------------------------------------
 * Asynchronous raw write (AkiraConsole)
 *
 * display_raw_write_async() queues the SPI transfer on the system work queue,
 * so the app renders its next frame while this one goes to the panel.  The
 * app must not touch the buffer until display_raw_wait() returns.  At most
 * one transfer is in flight.
 *
 * This is the one display path in which another thread reads app memory
 * after the native has returned (a frame is up to 150 KB: copying it would
 * defeat the call).  Ownership makes it safe: raw_async_busy holds the
 * submitting app's slot + 1 from the claim until the worker is done, and that
 * slot's release hook, which runs before the app's memory is freed, waits for
 * the worker or drops the queued job.  Every wait is bounded, and a stop ends
 * the app's own waits early.  A claim whose submitter died before queueing
 * (only a fault can do that: claim and submit form a critical span) is
 * detected and repaired by the next wait that times out, so neither the
 * shell nor the next app can hang on it.  One case is left to the app: if it
 * grows its linear memory while its own transfer is in flight, the worker
 * reads the old memory (a garbled frame on PSRAM boards).
 * -------------------------------------------------------------------------*/

static struct
{
    int x, y, w, h;
    const uint16_t *data;
} raw_async_job;
static atomic_t raw_async_busy;            /* owner slot + 1, 0 = idle */
static K_SEM_DEFINE(raw_async_done, 0, 1); /* wakes waiters early     */

#define RAW_WAIT_SLICE_MS 5
#define RAW_APP_WAIT_MS   1000 /* app side: a full frame is ~120 ms   */
#define RAW_SHELL_WAIT_MS 500  /* shell side: never freeze the UI on it */

static void raw_async_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    /* Window checked by the submitter; one panel's worth at most. */
    (void)akira_display_hal_write_raw(raw_async_job.x, raw_async_job.y,
                                      raw_async_job.w, raw_async_job.h,
                                      raw_async_job.data);
    atomic_clear(&raw_async_busy);
    k_sem_give(&raw_async_done);
}

static K_WORK_DEFINE(raw_async_work, raw_async_fn);

/* Clear claim @p owner if it has no transfer behind it (its submitter died
 * before k_work_submit()).  Call only when that submitter can't be mid-way:
 * its thread is gone, or a wait for it timed out. */
static void raw_async_repair(atomic_val_t owner)
{
    if (owner != 0 && !k_work_busy_get(&raw_async_work) &&
        atomic_cas(&raw_async_busy, owner, 0))
    {
        k_sem_give(&raw_async_done);
        LOG_WRN("display: dropped a raw-write claim left by a dead app thread");
    }
}

static inline bool raw_async_waiting_on(atomic_val_t cur, atomic_val_t owner)
{
    return cur != 0 && (owner == 0 || cur == owner);
}

/* Wait while a transfer of @p owner (slot + 1; 0 = anyone's) is in flight.
 * With an exec_env this is the app's own wait, which a stop ends early.
 * @return 0, -ECANCELED (the app is stopping) or -ETIMEDOUT. */
static int raw_async_wait(wasm_exec_env_t exec_env, atomic_val_t owner, int timeout_ms)
{
    int64_t deadline = k_uptime_get() + timeout_ms;

    for (;;)
    {
        atomic_val_t cur = atomic_get(&raw_async_busy);

        if (!raw_async_waiting_on(cur, owner))
            return 0;
        if (exec_env && akira_runtime_stop_requested(exec_env))
            return -ECANCELED;
        if (k_uptime_get() >= deadline)
        {
            /* Still claimed after the timeout with nothing queued or
             * running: a submitter preempted that long is a dead one. */
            raw_async_repair(cur);
            return raw_async_waiting_on(atomic_get(&raw_async_busy), owner) ? -ETIMEDOUT : 0;
        }
        (void)k_sem_take(&raw_async_done, K_MSEC(RAW_WAIT_SLICE_MS));
    }
}

/* The window rule akira_display_hal_write_raw() applies (entirely on the
 * panel), checked before queueing so the app gets the error. */
static int raw_window_check(int32_t x, int32_t y, int32_t w, int32_t h)
{
    int pw, ph;

    if (!panel_size(&pw, &ph))
        return -ENODEV;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        (int64_t)x + w > pw || (int64_t)y + h > ph)
        return -EINVAL;
    return 0;
}

/* Called by the OS shell before it draws over an app (and by the display
 * ownership code when it takes the display back).  Bounded. */
void akira_display_raw_async_wait(void)
{
    if (raw_async_wait(NULL, 0, RAW_SHELL_WAIT_MS) < 0)
        LOG_WRN("display: async raw write still in flight after %d ms", RAW_SHELL_WAIT_MS);
}

int akira_native_display_raw_write_async(wasm_exec_env_t exec_env,
                                         int32_t x, int32_t y, int32_t w, int32_t h,
                                         const uint8_t *data, uint32_t data_size)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    int ret = check_pixels(data, data_size, w, h);
    if (ret < 0)
        return ret;
    ret = raw_window_check(x, y, w, h);
    if (ret < 0)
        return ret;
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);

    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0)
        return -EACCES;
    const atomic_val_t me = (atomic_val_t)slot + 1;

    /* The previous transfer must be done. */
    ret = raw_async_wait(exec_env, 0, RAW_APP_WAIT_MS);
    if (ret < 0)
        return (ret == -ECANCELED) ? ret : -EBUSY;

    /* Claim and queue as one span: a stop can't abort the thread between
     * the two and leave a claim with nothing queued. */
    akira_native_critical_enter(exec_env);
    if (!atomic_cas(&raw_async_busy, 0, me))
    {
        akira_native_critical_exit(exec_env);
        return -EBUSY;
    }
    /* Re-check after claiming.  The shell flips the owner and then waits for
     * raw_async_busy == 0, so either it sees this claim or we see its flip. */
    if (!display_owner_is_wasm())
    {
        atomic_clear(&raw_async_busy);
        akira_native_critical_exit(exec_env);
        return -EBUSY;
    }
    raw_async_job.x = x;
    raw_async_job.y = y;
    raw_async_job.w = w;
    raw_async_job.h = h;
    raw_async_job.data = (const uint16_t *)data;
    if (k_work_submit(&raw_async_work) < 0)
    {
        atomic_clear(&raw_async_busy);
        ret = -EIO;
    }
    akira_native_critical_exit(exec_env);
    return ret;
}

int akira_native_display_raw_wait(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);

    int slot = akira_runtime_slot_of_exec_env(exec_env);
    if (slot < 0)
        return 0; /* not an app: it cannot have queued anything */

    /* Only this app's own transfer holds its buffer. */
    int ret = raw_async_wait(exec_env, (atomic_val_t)slot + 1, RAW_APP_WAIT_MS);

    return (ret == -ETIMEDOUT) ? -EBUSY : ret;
}

int akira_native_display_bitmap_transparent(wasm_exec_env_t exec_env,
                                            int32_t x, int32_t y, int32_t w, int32_t h,
                                            const uint8_t *data, uint32_t data_size, uint32_t key)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    int ret = check_pixels(data, data_size, w, h);
    if (ret < 0)
        return ret;
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_bitmap_transparent(x, y, w, h, (const uint16_t *)data, (uint16_t)key);
    schedule_auto_flush(exec_env);
    return 0;
}

/* Phase 4 native wrappers */

int akira_native_display_hline(wasm_exec_env_t exec_env,
                               int32_t x, int32_t y, int32_t len, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_hline(x, y, len, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_vline(wasm_exec_env_t exec_env,
                               int32_t x, int32_t y, int32_t len, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_vline(x, y, len, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_number(wasm_exec_env_t exec_env,
                                int32_t x, int32_t y, int32_t value, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_number(x, y, value, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_progress_bar(wasm_exec_env_t exec_env,
                                      int32_t x, int32_t y, int32_t w, int32_t h,
                                      int32_t value, int32_t max_val, uint32_t fg, uint32_t bg)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_progress_bar(x, y, w, h, value, max_val,
                               (uint16_t)fg, (uint16_t)bg);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_rounded_rect(wasm_exec_env_t exec_env,
                                      int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_rounded_rect(x, y, w, h, radius, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

int akira_native_display_rounded_rect_fill(wasm_exec_env_t exec_env,
                                           int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    AKIRA_CHECK_DISPLAY_OWNER_OR_RETURN(exec_env, -EBUSY);
    akira_display_rounded_rect_fill(x, y, w, h, radius, (uint16_t)color);
    schedule_auto_flush(exec_env);
    return 0;
}

/* ---------------------------------------------------------------------------
 * App teardown
 *
 * A draw call leaves behind a pending auto-flush, and an async raw write a
 * transfer that reads the app's memory; display ownership itself is dropped
 * by the runtime (akira_display_release_owner()), after this hook.  The flush
 * reads no app memory, so cancelling without waiting is enough: a flush
 * already running just presents the framebuffer once more.  The transfer
 * must be finished or dropped before the app's memory goes: waited for (at
 * most RAW_SHELL_WAIT_MS), or, on the system workqueue the worker runs on,
 * removed from the queue.  Idempotent, safe on any thread.
 * -------------------------------------------------------------------------*/

static void display_release(int slot, const char *app_name)
{
    ARG_UNUSED(app_name);
    if (atomic_cas(&g_auto_flush_slot, slot, -1))
        (void)k_work_cancel_delayable(&g_auto_flush_work);

    if (slot < 0)
        return;

    const atomic_val_t me = (atomic_val_t)slot + 1;

    if (atomic_get(&raw_async_busy) != me)
        return;

    if (k_current_get() == k_work_queue_thread_get(&k_sys_work_q))
    {
        /* The worker runs on this queue, so it is not running now: drop the
         * queued job (or the leaked claim) instead of waiting behind
         * ourselves. */
        if (k_work_cancel(&raw_async_work) == 0 && atomic_cas(&raw_async_busy, me, 0))
            k_sem_give(&raw_async_done);
        return;
    }

    /* The app's thread is gone or is this one, so no submitter of this slot
     * is mid-claim: a claim with nothing queued is a leaked one. */
    raw_async_repair(me);
    if (raw_async_wait(NULL, me, RAW_SHELL_WAIT_MS) < 0)
        LOG_ERR("display: raw write of app slot %d still running at teardown", slot);
}

AKIRA_APP_RESOURCE_DEFINE(display, 20, NULL, display_release);

#else /* !CONFIG_AKIRA_WASM_RUNTIME */

/* No app can have a transfer in flight.  The console shell calls this
 * unconditionally before it draws (the header declares it only with the
 * runtime). */
void akira_display_raw_async_wait(void);
void akira_display_raw_async_wait(void)
{
}

#endif /* CONFIG_AKIRA_WASM_RUNTIME */

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_DISPLAY))
#include <akira_native_registry.h>

static const NativeSymbol akira_display_natives[] = {
    {"display_rect", (void *)akira_native_display_rect, "(iiiii)i", NULL},
    {"display_text", (void *)akira_native_display_text, "(ii$i)i", NULL},
    {"display_text_large", (void *)akira_native_display_text_large, "(ii$i)i", NULL},
    {"display_text_huge", (void *)akira_native_display_text_huge, "(ii$i)i", NULL},
    {"display_clear", (void *)akira_native_display_clear, "(i)i", NULL},
    {"display_pixel", (void *)akira_native_display_pixel, "(iii)i", NULL},
    {"display_flush", (void *)akira_native_display_flush, "()i", NULL},
    {"display_get_size", (void *)akira_native_display_get_size, "(**)i", NULL},
    {"display_get_width", (void *)akira_native_display_get_width, "()i", NULL},
    {"display_get_height", (void *)akira_native_display_get_height, "()i", NULL},
    {"display_line", (void *)akira_native_display_line, "(iiiii)i", NULL},
    {"display_circle", (void *)akira_native_display_circle, "(iiii)i", NULL},
    {"display_circle_fill", (void *)akira_native_display_circle_fill, "(iiii)i", NULL},
    {"display_triangle", (void *)akira_native_display_triangle, "(iiiiiii)i", NULL},
    {"display_triangle_fill", (void *)akira_native_display_triangle_fill, "(iiiiiii)i", NULL},
    {"display_rect_outline", (void *)akira_native_display_rect_outline, "(iiiii)i", NULL},
    {"display_bitmap", (void *)akira_native_display_bitmap, "(iiii*~)i", NULL},
    {"display_bitmap_transparent", (void *)akira_native_display_bitmap_transparent, "(iiii*~i)i", NULL},
    {"display_raw_write", (void *)akira_native_display_raw_write, "(iiii*~)i", NULL},
    {"display_raw_write_async", (void *)akira_native_display_raw_write_async, "(iiii*~)i", NULL},
    {"display_raw_wait", (void *)akira_native_display_raw_wait, "()i", NULL},
    /* Phase 4 — UI helper primitives */
    {"display_hline", (void *)akira_native_display_hline, "(iiii)i", NULL},
    {"display_vline", (void *)akira_native_display_vline, "(iiii)i", NULL},
    {"display_number", (void *)akira_native_display_number, "(iiii)i", NULL},
    {"display_progress_bar", (void *)akira_native_display_progress_bar, "(iiiiiiii)i", NULL},
    {"display_rounded_rect", (void *)akira_native_display_rounded_rect, "(iiiiii)i", NULL},
    {"display_rounded_rect_fill", (void *)akira_native_display_rounded_rect_fill, "(iiiiii)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_display_api, "env", akira_display_natives);
#endif
