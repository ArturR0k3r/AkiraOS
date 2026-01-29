/**
 * @file lvgl_display_driver.c
 * @brief LVGL display driver for ILI9341
 * 
 * Integrates LVGL graphics library with ILI9341 TFT display.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_LVGL)
#include <lvgl.h>
#include "display_ili9341.h"

LOG_MODULE_REGISTER(lvgl_display, LOG_LEVEL_INF);

/* Display resolution */
#define LVGL_HOR_RES ILI9341_DISPLAY_WIDTH   // 320
#define LVGL_VER_RES ILI9341_DISPLAY_HEIGHT  // 240

/* Buffer size (10% of screen) */
#define BUFFER_SIZE (LVGL_HOR_RES * LVGL_VER_RES / 10)

/* Display buffers for double buffering */
static lv_color_t buf1[BUFFER_SIZE];
static lv_color_t buf2[BUFFER_SIZE];

static lv_disp_drv_t disp_drv;
static lv_disp_draw_buf_t disp_buf;
static lv_disp_t *display;

/**
 * @brief Flush callback - called by LVGL to update screen region
 * 
 * @param drv Display driver
 * @param area Screen area to update
 * @param color_p Pixel data buffer
 */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    int32_t x = area->x1;
    int32_t y = area->y1;
    int32_t width = area->x2 - area->x1 + 1;
    int32_t height = area->y2 - area->y1 + 1;
    
    /* Set drawing window */
    ili9341_set_window(x, y, x + width - 1, y + height - 1);
    
    /* Send pixel data to display */
    size_t pixel_count = width * height;
    ili9341_write_data((uint16_t *)color_p, pixel_count);
    
    /* Tell LVGL flush is complete */
    lv_disp_flush_ready(drv);
}

/**
 * @brief Initialize LVGL display driver
 * 
 * @return 0 on success, negative error code on failure
 */
int lvgl_display_init(void)
{
    /* Initialize LVGL library */
    lv_init();
    LOG_INF("LVGL v%d.%d.%d initialized", 
            lv_version_major(), lv_version_minor(), lv_version_patch());
    
    /* Initialize display hardware */
    int ret = ili9341_init();
    if (ret < 0) {
        LOG_ERR("Failed to initialize ILI9341: %d", ret);
        return ret;
    }
    
    /* Initialize display buffers */
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, BUFFER_SIZE);
    LOG_INF("LVGL display buffers: 2x%d pixels (%d KB each)",
            BUFFER_SIZE, (BUFFER_SIZE * sizeof(lv_color_t)) / 1024);
    
    /* Initialize display driver */
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &disp_buf;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.hor_res = LVGL_HOR_RES;
    disp_drv.ver_res = LVGL_VER_RES;
    
    /* Register display driver */
    display = lv_disp_drv_register(&disp_drv);
    if (!display) {
        LOG_ERR("Failed to register LVGL display driver");
        return -ENOMEM;
    }
    
    LOG_INF("LVGL display driver registered (%dx%d)", LVGL_HOR_RES, LVGL_VER_RES);
    return 0;
}

/**
 * @brief LVGL tick handler (should be called every 1ms)
 */
static void lvgl_tick_handler(struct k_timer *timer)
{
    lv_tick_inc(1);
}

K_TIMER_DEFINE(lvgl_timer, lvgl_tick_handler, NULL);

/**
 * @brief Start LVGL tick timer
 */
void lvgl_start_tick(void)
{
    k_timer_start(&lvgl_timer, K_MSEC(1), K_MSEC(1));
    LOG_INF("LVGL tick timer started (1ms interval)");
}

/**
 * @brief Get LVGL display object
 * 
 * @return Display object pointer
 */
lv_disp_t *lvgl_get_display(void)
{
    return display;
}

#else /* !CONFIG_LVGL */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lvgl_display, LOG_LEVEL_WRN);

int lvgl_display_init(void)
{
    LOG_WRN("LVGL not enabled in Kconfig");
    return -ENOTSUP;
}

void lvgl_start_tick(void)
{
}

void *lvgl_get_display(void)
{
    return NULL;
}

#endif /* CONFIG_LVGL */
