/**
 * @file akira_display_api.c
 * @brief Display API implementation for WASM exports
 */

#include "akira_api.h"
#include "../drivers/display_ili9341.h"
#include "../drivers/akira_hal.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_display_api, LOG_LEVEL_INF);

// TODO: Add capability check before each API call
// TODO: Add framebuffer double-buffering support
// TODO: Add display rotation support
// TODO: Add clipping rectangle support

void akira_display_clear(uint16_t color)
{
    // TODO: Check CAP_DISPLAY_WRITE capability
    // TODO: Implement using display driver
    LOG_DBG("display_clear(0x%04X)", color);
}

void akira_display_pixel(int x, int y, uint16_t color)
{
    // TODO: Check CAP_DISPLAY_WRITE capability
    // TODO: Bounds checking
    // TODO: Implement using display driver or framebuffer
    (void)x;
    (void)y;
    (void)color;
}

void akira_display_rect(int x, int y, int w, int h, uint16_t color)
{
    // TODO: Check CAP_DISPLAY_WRITE capability
    // TODO: Implement filled rectangle
    // TODO: Optimize with DMA transfer for large rects
    LOG_DBG("display_rect(%d,%d,%d,%d,0x%04X)", x, y, w, h, color);
}

void akira_display_text(int x, int y, const char *text, uint16_t color)
{
    // TODO: Check CAP_DISPLAY_WRITE capability
    // TODO: Implement font rendering
    // TODO: Support multiple font sizes
    // TODO: Support UTF-8
    LOG_DBG("display_text(%d,%d,'%s',0x%04X)", x, y, text ? text : "NULL", color);
}

void akira_display_flush(void)
{
    // TODO: Check CAP_DISPLAY_WRITE capability
    // TODO: Flush framebuffer to physical display
    // TODO: Implement vsync if supported
    LOG_DBG("display_flush()");
}

void akira_display_get_size(int *width, int *height)
{
    // TODO: Return actual display dimensions from driver
    if (width)
        *width = 320;
    if (height)
        *height = 240;
}
