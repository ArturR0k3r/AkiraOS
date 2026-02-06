/**
 * @file akira_display_api.h
 * @brief Display API declarations for WASM exports
 */

#ifndef AKIRA_DISPLAY_API_H
#define AKIRA_DISPLAY_API_H

#include <stdint.h>
#include <wasm_export.h>

/* Core display API functions (no security checks) */
void akira_display_clear(uint16_t color);
void akira_display_pixel(int x, int y, uint16_t color);
void akira_display_rect(int x, int y, int w, int h, uint16_t color);
void akira_display_text(int x, int y, const char *text, uint16_t color);
void akira_display_text_large(int x, int y, const char *text, uint16_t color);
void akira_display_flush(void);
void akira_display_get_size(int *width, int *height);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM native export functions (with capability checks) */
int akira_native_display_clear(wasm_exec_env_t exec_env, uint32_t color);
int akira_native_display_pixel(wasm_exec_env_t exec_env, int32_t x, int32_t y, uint32_t color);
int akira_native_display_rect(wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
int akira_native_display_text(wasm_exec_env_t exec_env, int32_t x, int32_t y, const char *text, uint32_t color);
int akira_native_display_text_large(wasm_exec_env_t exec_env, int x, int y, const char *text, uint16_t color);
#endif

#endif /* AKIRA_DISPLAY_API_H */