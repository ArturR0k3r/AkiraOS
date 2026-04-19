/*
 * Copyright (c) 2026 PenEngineering S.R.L
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file akira_display_api.h
 * @brief Display API declarations for WASM native exports.
 */

#ifndef AKIRA_DISPLAY_API_H
#define AKIRA_DISPLAY_API_H

#include <stdint.h>
#include <wasm_export.h>

/* ---------------------------------------------------------------------------
 * Platform-agnostic primitives (no security checks)
 * All draw calls write to the framebuffer only.  Call akira_display_flush()
 * to push the framebuffer to the hardware driver.
 * -------------------------------------------------------------------------*/
void akira_display_clear(uint16_t color);
void akira_display_pixel(int x, int y, uint16_t color);
void akira_display_rect(int x, int y, int w, int h, uint16_t color);
void akira_display_text(int x, int y, const char *text, uint16_t color);
void akira_display_text_large(int x, int y, const char *text, uint16_t color);
void akira_display_flush(void);
void akira_display_get_size(int *width, int *height);

/* Phase 3.5 — Additional primitives */
void akira_display_line(int x0, int y0, int x1, int y1, uint16_t color);
void akira_display_circle(int cx, int cy, int r, uint16_t color);
void akira_display_circle_fill(int cx, int cy, int r, uint16_t color);
void akira_display_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);
void akira_display_triangle_fill(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);
void akira_display_rect_outline(int x, int y, int w, int h, uint16_t color);
void akira_display_bitmap(int x, int y, int w, int h, const uint16_t *data);
void akira_display_bitmap_transparent(int x, int y, int w, int h,
                                       const uint16_t *data, uint16_t key);

/* Phase 4 — UI helper primitives */
void akira_display_hline(int x, int y, int len, uint16_t color);
void akira_display_vline(int x, int y, int len, uint16_t color);
void akira_display_number(int x, int y, int32_t value, uint16_t color);
void akira_display_progress_bar(int x, int y, int w, int h,
                                 int32_t value, int32_t max_val,
                                 uint16_t fg, uint16_t bg);
void akira_display_rounded_rect(int x, int y, int w, int h,
                                 int radius, uint16_t color);
void akira_display_rounded_rect_fill(int x, int y, int w, int h,
                                      int radius, uint16_t color);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* ---------------------------------------------------------------------------
 * WASM native export wrappers (with capability checks)
 * Registered in akira_export_api.c.
 * -------------------------------------------------------------------------*/
int akira_native_display_clear(wasm_exec_env_t exec_env, uint32_t color);
int akira_native_display_pixel(wasm_exec_env_t exec_env, int32_t x, int32_t y, uint32_t color);
int akira_native_display_rect(wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
int akira_native_display_text(wasm_exec_env_t exec_env, int32_t x, int32_t y, const char *text, uint32_t color);
int akira_native_display_text_large(wasm_exec_env_t exec_env, int x, int y, const char *text, uint32_t color);
int akira_native_display_flush(wasm_exec_env_t exec_env);
int akira_native_display_get_size(wasm_exec_env_t exec_env, int32_t *w_out, int32_t *h_out);

/* Phase 3.5 native wrappers */
int akira_native_display_line(wasm_exec_env_t exec_env, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
int akira_native_display_circle(wasm_exec_env_t exec_env, int32_t cx, int32_t cy, int32_t r, uint32_t color);
int akira_native_display_circle_fill(wasm_exec_env_t exec_env, int32_t cx, int32_t cy, int32_t r, uint32_t color);
int akira_native_display_triangle(wasm_exec_env_t exec_env, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color);
int akira_native_display_triangle_fill(wasm_exec_env_t exec_env, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color);
int akira_native_display_rect_outline(wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
int akira_native_display_bitmap(wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *data, uint32_t data_size);
int akira_native_display_bitmap_transparent(wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *data, uint32_t data_size, uint32_t key);
int akira_native_display_raw_write(wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *data, uint32_t data_size);

/* Phase 4 native wrappers */
int akira_native_display_hline(wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t len, uint32_t color);
int akira_native_display_vline(wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t len, uint32_t color);
int akira_native_display_number(wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t value, uint32_t color);
int akira_native_display_progress_bar(wasm_exec_env_t exec_env,
    int32_t x, int32_t y, int32_t w, int32_t h,
    int32_t value, int32_t max_val, uint32_t fg, uint32_t bg);
int akira_native_display_rounded_rect(wasm_exec_env_t exec_env,
    int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color);
int akira_native_display_rounded_rect_fill(wasm_exec_env_t exec_env,
    int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color);
#endif /* CONFIG_AKIRA_WASM_RUNTIME */

#endif /* AKIRA_DISPLAY_API_H */