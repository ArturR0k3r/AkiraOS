/**
 * @file akira_display_api.c
 * @brief Display API implementation for WASM exports
 */
#include "akira_api.h"
#include <runtime/security.h>
#include <drivers/platform_hal.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_display_api, LOG_LEVEL_INF);

/* Platform-agnostic display primitives. All hardware calls go through
 * platform_hal.h. No security checks here - only in native exports.
 */

void akira_display_clear(uint16_t color)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    for (int y = 0; y < 320; y++)
        for (int x = 0; x < 240; x++)
            akira_sim_draw_pixel(x, y, color);
    akira_sim_show_display();
#else
    uint16_t *fb = akira_framebuffer_get();
    if (fb)
    {
        for (int i = 0; i < 240 * 320; i++)
            fb[i] = color;
    }
    else
    {
        LOG_WRN("No display framebuffer available");
    }
#endif
}

void akira_display_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= 240 || y < 0 || y >= 320)
        return;

#if AKIRA_PLATFORM_NATIVE_SIM
    akira_sim_draw_pixel(x, y, color);
#else
    uint16_t *fb = akira_framebuffer_get();
    if (fb)
        fb[y * 240 + x] = color;
#endif
}

void akira_display_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0)
        return;
    
    int x_end = x + w;
    int y_end = y + h;
    for (int py = y; py < y_end; py++)
    {
        for (int px = x; px < x_end; px++)
        {
            akira_display_pixel(px, py, color);
        }
    }
}

void akira_display_text(int x, int y, const char *text, uint16_t color)
{
    (void)x; (void)y; (void)text; (void)color;
    LOG_WRN("akira_display_text: not implemented in minimal API");
}

void akira_display_text_large(int x, int y, const char *text, uint16_t color)
{
    (void)x; (void)y; (void)text; (void)color;
    LOG_WRN("akira_display_text_large: not implemented in minimal API");
}

void akira_display_flush(void)
{
#if AKIRA_PLATFORM_NATIVE_SIM
    akira_sim_show_display();
#endif
}

void akira_display_get_size(int *width, int *height)
{
    if (width) *width = 240;
    if (height) *height = 320;
}

/* WASM Native export API */

#ifdef CONFIG_AKIRA_WASM_RUNTIME

int akira_native_display_rect(wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    /* Use inline capability check for <60ns overhead */
    uint32_t cap_mask = akira_security_get_cap_mask(exec_env);
    AKIRA_CHECK_CAP_OR_RETURN(cap_mask, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    
    akira_display_rect(x, y, w, h, (uint16_t)color);
    return 0;
}

int akira_native_display_text(wasm_exec_env_t exec_env, int32_t x, int32_t y, const char *text, uint32_t color)
{
    /* Use inline capability check for <60ns overhead */
    uint32_t cap_mask = akira_security_get_cap_mask(exec_env);
    AKIRA_CHECK_CAP_OR_RETURN(cap_mask, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    
    akira_display_text(x, y, text, (uint16_t)color);
    return 0;
}

int akira_native_display_text_large(wasm_exec_env_t exec_env, int x, int y, const char *text, uint16_t color)
{
    /* Use inline capability check for <60ns overhead */
    uint32_t cap_mask = akira_security_get_cap_mask(exec_env);
    AKIRA_CHECK_CAP_OR_RETURN(cap_mask, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    
    akira_display_text_large(x, y, text, color);
    return 0;
}

int akira_native_display_clear(wasm_exec_env_t exec_env, uint32_t color)
{
    /* Use inline capability check for <60ns overhead */
    uint32_t cap_mask = akira_security_get_cap_mask(exec_env);
    AKIRA_CHECK_CAP_OR_RETURN(cap_mask, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    
    akira_display_clear((uint16_t)color);
    return 0;
}

int akira_native_display_pixel(wasm_exec_env_t exec_env, int32_t x, int32_t y, uint32_t color)
{
    /* Use inline capability check for <60ns overhead */
    uint32_t cap_mask = akira_security_get_cap_mask(exec_env);
    AKIRA_CHECK_CAP_OR_RETURN(cap_mask, AKIRA_CAP_DISPLAY_WRITE, -EPERM);
    
    akira_display_pixel(x, y, (uint16_t)color);
    return 0;
}

#endif