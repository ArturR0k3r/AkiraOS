/**
 * @file akira_display_api.c
 * @brief Display API implementation for WASM exports
 */

#include "akira_api.h"
#include "../drivers/display_ili9341.h"
#include "../drivers/fonts.h"
#include "../drivers/platform_hal.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_display_api, LOG_LEVEL_INF);

// TODO: Add capability check before each API call 
// TODO: Add framebuffer double-buffering support
// TODO: Add display rotation support
// TODO: Add clipping rectangle support

void akira_display_clear(uint16_t color)
{
    // TODO: Check CAP_DISPLAY_WRITE capability 
    
#ifdef CONFIG_ILI9341_DISPLAY
    ili9341_fill_color(color);
#else
    LOG_WRN("No display driver configured");
#endif
}

void akira_display_pixel(int x, int y, uint16_t color)
{
    // TODO: Check CAP_DISPLAY_WRITE capability 
    
    // Bounds checking
    if (x < 0 || x >= ILI9341_DISPLAY_WIDTH || y < 0 || y >= ILI9341_DISPLAY_HEIGHT) {
        return;
    }
    
#ifdef CONFIG_ILI9341_DISPLAY
    ili9341_draw_pixel(x, y, color);
#else
    LOG_WRN("No display driver configured");
#endif
}

void akira_display_rect(int x, int y, int w, int h, uint16_t color)
{
    // TODO: Check CAP_DISPLAY_WRITE capability 
    
    // Bounds validation
    if (x < 0 || y < 0 || w <= 0 || h <= 0) {
        return;
    }
    
    // Clip to display bounds
    if (x >= ILI9341_DISPLAY_WIDTH || y >= ILI9341_DISPLAY_HEIGHT) {
        return;
    }
    
    int x_end = (x + w > ILI9341_DISPLAY_WIDTH) ? ILI9341_DISPLAY_WIDTH : x + w;
    int y_end = (y + h > ILI9341_DISPLAY_HEIGHT) ? ILI9341_DISPLAY_HEIGHT : y + h;
    
#ifdef CONFIG_ILI9341_DISPLAY
    // Draw filled rectangle pixel by pixel
    // TODO: Optimize with DMA transfer for large rects
    for (int py = y; py < y_end; py++) {
        for (int px = x; px < x_end; px++) {
            ili9341_draw_pixel(px, py, color);
        }
    }
#else
    LOG_WRN("No display driver configured");
#endif
}

void akira_display_text(int x, int y, const char *text, uint16_t color)
{
    // TODO: Check CAP_DISPLAY_WRITE capability 
    
    if (!text) {
        return;
    }
    
#ifdef CONFIG_ILI9341_DISPLAY
    // Use 7x10 font by default
    ili9341_draw_text(x, y, text, color, FONT_7X10);
#else
    LOG_WRN("No display driver configured");
#endif
}

void akira_display_text_large(int x, int y, const char *text, uint16_t color)
{
    // TODO: Check CAP_DISPLAY_WRITE capability 
    
    if (!text) {
        return;
    }
    
#ifdef CONFIG_ILI9341_DISPLAY
    // Use 11x18 font for large text
    ili9341_draw_text(x, y, text, color, FONT_11X18);
#else
    LOG_WRN("No display driver configured");
#endif
}

void akira_display_flush(void)
{
    // TODO: Check CAP_DISPLAY_WRITE capability 
    // TODO: Flush framebuffer to physical display if double-buffering enabled
    // TODO: Implement vsync if supported
    
    // Currently no-op as ILI9341 updates are immediate
    // This is a placeholder for future framebuffer implementation
}

void akira_display_get_size(int *width, int *height)
{
    if (width) {
        *width = ILI9341_DISPLAY_WIDTH;
    }
    if (height) {
        *height = ILI9341_DISPLAY_HEIGHT;
    }
}
