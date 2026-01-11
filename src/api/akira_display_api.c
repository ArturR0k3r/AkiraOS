/**
 * @file akira_display_api.c
 * @brief Minimal Display API - Direct framebuffer access for WASM
 * 
 * Only basic primitives. For complex UIs, WASM apps should use LVGL directly.
 */

#include "akira_api.h"
#include "../drivers/display_ili9341.h"
#include <wasm_export.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_display_api, LOG_LEVEL_INF);

/*===========================================================================*/
/* Native C API                                                              */
/*===========================================================================*/

void akira_display_clear(uint16_t color)
{
#ifdef CONFIG_ILI9341_DISPLAY
    ili9341_fill_color(color);
#else
    LOG_WRN("Display not configured");
#endif
}

void akira_display_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= ILI9341_DISPLAY_WIDTH || y < 0 || y >= ILI9341_DISPLAY_HEIGHT)
        return;
    
#ifdef CONFIG_ILI9341_DISPLAY
    ili9341_draw_pixel(x, y, color);
#endif
}

void akira_display_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0 || x >= ILI9341_DISPLAY_WIDTH || y >= ILI9341_DISPLAY_HEIGHT)
        return;
    
    int x_end = (x + w > ILI9341_DISPLAY_WIDTH) ? ILI9341_DISPLAY_WIDTH : x + w;
    int y_end = (y + h > ILI9341_DISPLAY_HEIGHT) ? ILI9341_DISPLAY_HEIGHT : y + h;
    
    for (int py = (y < 0 ? 0 : y); py < y_end; py++) {
        for (int px = (x < 0 ? 0 : x); px < x_end; px++) {
            akira_display_pixel(px, py, color);
        }
    }
}

void akira_display_get_size(int *width, int *height)
{
    if (width) *width = ILI9341_DISPLAY_WIDTH;
    if (height) *height = ILI9341_DISPLAY_HEIGHT;
}

/*===========================================================================*/
/* WASM Wrappers (exported to OCRE)                                          */
/*===========================================================================*/

int akira_display_clear_wasm(wasm_exec_env_t exec_env, int color)
{
    (void)exec_env;
    akira_display_clear((uint16_t)color);
    return 0;
}

int akira_display_pixel_wasm(wasm_exec_env_t exec_env, int x, int y, int color)
{
    (void)exec_env;
    akira_display_pixel(x, y, (uint16_t)color);
    return 0;
}

int akira_display_rect_wasm(wasm_exec_env_t exec_env, int x, int y, int w, int h, int color)
{
    (void)exec_env;
    akira_display_rect(x, y, w, h, (uint16_t)color);
    return 0;
}

int akira_display_get_size_wasm(wasm_exec_env_t exec_env, uint32_t width_ptr, uint32_t height_ptr)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
        return -1;
    
    int *w = (int *)wasm_runtime_addr_app_to_native(module_inst, width_ptr);
    int *h = (int *)wasm_runtime_addr_app_to_native(module_inst, height_ptr);
    if (!w || !h)
        return -1;
    
    akira_display_get_size(w, h);
    return 0;
}
