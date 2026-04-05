/*
 * AkiraConsole Simulator — WAMR native symbol registration
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Registers the same function names that the AkiraSDK WASM apps import,
 * backed by the SDL2 / POSIX host implementations.
 *
 * Keep function signatures in sync with src/api/akira_export_api.c.
 */

#include "wamr_host.h"
#include "../host/sim_display.h"
#include "../host/sim_input.h"
#include "../host/sim_storage.h"

#include <wasm_export.h>
#include <stdint.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Forward declarations from host/sim_log.c and host/sim_time.c
 * --------------------------------------------------------------------- */
extern int sim_log_printf(wasm_exec_env_t, const char *);
extern int sim_delay(wasm_exec_env_t, int);
extern int sim_time_ms(wasm_exec_env_t);
extern int sim_time_us(wasm_exec_env_t);
extern int sim_time_unix(wasm_exec_env_t);

/* -----------------------------------------------------------------------
 * Display wrappers — strip exec_env, delegate to sim_display_*
 * --------------------------------------------------------------------- */

static int w_display_clear(wasm_exec_env_t e, uint32_t c)
    { (void)e; sim_display_clear((uint16_t)c); return 0; }
static int w_display_pixel(wasm_exec_env_t e, int32_t x, int32_t y, uint32_t c)
    { (void)e; sim_display_pixel(x, y, (uint16_t)c); return 0; }
static int w_display_rect(wasm_exec_env_t e, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c)
    { (void)e; sim_display_rect(x, y, w, h, (uint16_t)c); return 0; }
static int w_display_text(wasm_exec_env_t e, int32_t x, int32_t y, const char *t, uint32_t c)
    { (void)e; sim_display_text(x, y, t, (uint16_t)c); return 0; }
static int w_display_text_large(wasm_exec_env_t e, int32_t x, int32_t y, const char *t, uint32_t c)
    { (void)e; sim_display_text_large(x, y, t, (uint16_t)c); return 0; }
static int w_display_flush(wasm_exec_env_t e)
    { (void)e; /* render happens in main loop */ return 0; }
static int w_display_get_size(wasm_exec_env_t e, int32_t *wo, int32_t *ho)
    { (void)e; if (wo) *wo = DISP_W; if (ho) *ho = DISP_H; return 0; }
static int w_display_line(wasm_exec_env_t e, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c)
    { (void)e; sim_display_line(x0, y0, x1, y1, (uint16_t)c); return 0; }
static int w_display_circle(wasm_exec_env_t e, int32_t cx, int32_t cy, int32_t r, uint32_t c)
    { (void)e; sim_display_circle(cx, cy, r, (uint16_t)c); return 0; }
static int w_display_circle_fill(wasm_exec_env_t e, int32_t cx, int32_t cy, int32_t r, uint32_t c)
    { (void)e; sim_display_circle_fill(cx, cy, r, (uint16_t)c); return 0; }
static int w_display_triangle(wasm_exec_env_t e,
    int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t c)
    { (void)e; sim_display_triangle(x0,y0,x1,y1,x2,y2,(uint16_t)c); return 0; }
static int w_display_triangle_fill(wasm_exec_env_t e,
    int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t c)
    { (void)e; sim_display_triangle_fill(x0,y0,x1,y1,x2,y2,(uint16_t)c); return 0; }
static int w_display_rect_outline(wasm_exec_env_t e, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c)
    { (void)e; sim_display_rect_outline(x,y,w,h,(uint16_t)c); return 0; }
static int w_display_bitmap(wasm_exec_env_t e,
    int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *data, uint32_t sz)
    { (void)e; (void)sz; sim_display_bitmap(x,y,w,h,(const uint16_t*)data); return 0; }
static int w_display_bitmap_transparent(wasm_exec_env_t e,
    int32_t x, int32_t y, int32_t w, int32_t h,
    const uint8_t *data, uint32_t sz, uint32_t key)
    { (void)e; (void)sz; sim_display_bitmap_transparent(x,y,w,h,(const uint16_t*)data,(uint16_t)key); return 0; }
static int w_display_raw_write(wasm_exec_env_t e,
    int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *data, uint32_t sz)
    { (void)e; (void)sz; sim_display_bitmap(x,y,w,h,(const uint16_t*)data); return 0; }
static int w_display_hline(wasm_exec_env_t e, int32_t x, int32_t y, int32_t len, uint32_t c)
    { (void)e; sim_display_hline(x,y,len,(uint16_t)c); return 0; }
static int w_display_vline(wasm_exec_env_t e, int32_t x, int32_t y, int32_t len, uint32_t c)
    { (void)e; sim_display_vline(x,y,len,(uint16_t)c); return 0; }
static int w_display_number(wasm_exec_env_t e, int32_t x, int32_t y, int32_t v, uint32_t c)
    { (void)e; sim_display_number(x,y,v,(uint16_t)c); return 0; }
static int w_display_progress_bar(wasm_exec_env_t e,
    int32_t x, int32_t y, int32_t w, int32_t h,
    int32_t val, int32_t max, uint32_t fg, uint32_t bg)
    { (void)e; sim_display_progress_bar(x,y,w,h,val,max,(uint16_t)fg,(uint16_t)bg); return 0; }
static int w_display_rounded_rect(wasm_exec_env_t e,
    int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t c)
    { (void)e; sim_display_rounded_rect(x,y,w,h,r,(uint16_t)c); return 0; }
static int w_display_rounded_rect_fill(wasm_exec_env_t e,
    int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t c)
    { (void)e; sim_display_rounded_rect_fill(x,y,w,h,r,(uint16_t)c); return 0; }

/* -----------------------------------------------------------------------
 * Input wrappers
 * --------------------------------------------------------------------- */

static int w_input_get_buttons(wasm_exec_env_t e)
    { (void)e; return (int)sim_input_get_bitmask(); }

static int w_input_poll_event(wasm_exec_env_t e, uint32_t ptr, uint32_t len)
{
    (void)e;
    if (len < 8) return -1;
    /* Resolve WASM linear memory pointer */
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(e);
    void *native_ptr = wasm_runtime_addr_app_to_native(inst, ptr);
    if (!native_ptr) return -1;
    sim_input_event_t ev;
    if (!sim_input_poll_event(&ev)) return 0;
    memcpy(native_ptr, &ev, 8);
    return 1;
}

/* -----------------------------------------------------------------------
 * Storage wrappers
 * --------------------------------------------------------------------- */

static int w_storage_open(wasm_exec_env_t e, const char *path, int flags)
    { (void)e; return sim_storage_open(path, flags); }
static int w_storage_read(wasm_exec_env_t e, int fd, uint8_t *buf, uint32_t len)
    { (void)e; return sim_storage_read(fd, buf, (int)len); }
static int w_storage_write(wasm_exec_env_t e, int fd, const uint8_t *buf, uint32_t len)
    { (void)e; return sim_storage_write(fd, buf, (int)len); }
static void w_storage_close(wasm_exec_env_t e, int fd)
    { (void)e; sim_storage_close(fd); }
static int w_storage_delete(wasm_exec_env_t e, const char *path)
    { (void)e; return sim_storage_delete(path); }
static int w_storage_list(wasm_exec_env_t e, const char *dir, char *out, uint32_t len)
    { (void)e; return sim_storage_list(dir, out, (int)len); }

/* -----------------------------------------------------------------------
 * Native symbol table — must match akira_export_api.c signatures exactly
 * --------------------------------------------------------------------- */

static NativeSymbol g_symbols[] = {
    /* Core */
    { "printf_native",              (void *)sim_log_printf,              "($)i",        NULL },
    { "delay",                      (void *)sim_delay,                   "(i)i",        NULL },

    /* Display */
    { "display_clear",              (void *)w_display_clear,             "(i)i",        NULL },
    { "display_pixel",              (void *)w_display_pixel,             "(iii)i",      NULL },
    { "display_rect",               (void *)w_display_rect,              "(iiiii)i",    NULL },
    { "display_text",               (void *)w_display_text,              "(ii$i)i",     NULL },
    { "display_text_large",         (void *)w_display_text_large,        "(ii$i)i",     NULL },
    { "display_flush",              (void *)w_display_flush,             "()i",         NULL },
    { "display_get_size",           (void *)w_display_get_size,          "(**)i",       NULL },
    { "display_line",               (void *)w_display_line,              "(iiiii)i",    NULL },
    { "display_circle",             (void *)w_display_circle,            "(iiii)i",     NULL },
    { "display_circle_fill",        (void *)w_display_circle_fill,       "(iiii)i",     NULL },
    { "display_triangle",           (void *)w_display_triangle,          "(iiiiiii)i",  NULL },
    { "display_triangle_fill",      (void *)w_display_triangle_fill,     "(iiiiiii)i",  NULL },
    { "display_rect_outline",       (void *)w_display_rect_outline,      "(iiiii)i",    NULL },
    { "display_bitmap",             (void *)w_display_bitmap,            "(iiii*~)i",   NULL },
    { "display_bitmap_transparent", (void *)w_display_bitmap_transparent,"(iiii*~i)i",  NULL },
    { "display_raw_write",          (void *)w_display_raw_write,         "(iiii*~)i",   NULL },
    { "display_hline",              (void *)w_display_hline,             "(iiii)i",     NULL },
    { "display_vline",              (void *)w_display_vline,             "(iiii)i",     NULL },
    { "display_number",             (void *)w_display_number,            "(iiii)i",     NULL },
    { "display_progress_bar",       (void *)w_display_progress_bar,      "(iiiiiiii)i", NULL },
    { "display_rounded_rect",       (void *)w_display_rounded_rect,      "(iiiiii)i",   NULL },
    { "display_rounded_rect_fill",  (void *)w_display_rounded_rect_fill, "(iiiiii)i",   NULL },

    /* Input */
    { "input_get_buttons",          (void *)w_input_get_buttons,         "()i",         NULL },
    { "input_poll_event",           (void *)w_input_poll_event,          "(*~)i",       NULL },

    /* Storage (SD card) */
    { "storage_open",               (void *)w_storage_open,              "($i)i",       NULL },
    { "storage_read",               (void *)w_storage_read,              "(i*~)i",      NULL },
    { "storage_write",              (void *)w_storage_write,             "(i*~)i",      NULL },
    { "storage_close",              (void *)w_storage_close,             "(i)",         NULL },
    { "storage_delete",             (void *)w_storage_delete,            "($)i",        NULL },
    { "storage_list",               (void *)w_storage_list,              "($*~)i",      NULL },

    /* Time */
    { "time_ms",                    (void *)sim_time_ms,                 "()i",         NULL },
    { "time_us",                    (void *)sim_time_us,                 "()i",         NULL },
    { "time_unix",                  (void *)sim_time_unix,               "()i",         NULL },
};

bool wamr_host_register_natives(void)
{
    int n = (int)(sizeof(g_symbols) / sizeof(g_symbols[0]));
    return wasm_runtime_register_natives("env", g_symbols, (uint32_t)n);
}
