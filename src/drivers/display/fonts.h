/**
 * @file fonts.h
 * @stability experimental
 * @since 1.4
 */
#ifndef __FONTS_H__
#define __FONTS_H__

#include <stdint.h>

#define FONT7X10_WIDTH 7
#define FONT7X10_HEIGHT 10
#define FONT7X10_FIRST_CHAR 32
#define FONT7X10_LAST_CHAR 126
#define FONT7X10_NUM_CHARS (FONT7X10_LAST_CHAR - FONT7X10_FIRST_CHAR + 1)

#define FONT11X18_WIDTH 11
#define FONT11X18_HEIGHT 18
#define FONT11X18_FIRST_CHAR 32
#define FONT11X18_LAST_CHAR 126
#define FONT11X18_NUM_CHARS (FONT11X18_LAST_CHAR - FONT11X18_FIRST_CHAR + 1)

#define FONT16X28_WIDTH 16
#define FONT16X28_HEIGHT 28
#define FONT16X28_FIRST_CHAR 32
#define FONT16X28_LAST_CHAR 126
#define FONT16X28_NUM_CHARS (FONT16X28_LAST_CHAR - FONT16X28_FIRST_CHAR + 1)

extern const uint16_t font7x10[FONT7X10_NUM_CHARS][FONT7X10_HEIGHT];
extern const uint16_t font11x18[FONT11X18_NUM_CHARS][FONT11X18_HEIGHT];
extern const uint16_t font_hero[FONT16X28_NUM_CHARS][FONT16X28_HEIGHT];

typedef enum
{
    FONT_7X10,
    FONT_11X18,
    FONT_HERO /* 16x28 — the one tuned/read value per screen */
} FontType;

// Fixed function declarations to match implementation
void draw_string(int x, int y, const char *str, uint16_t color, void (*set_pixel)(int, int, uint16_t), FontType font);
void draw_char(int x, int y, char c, uint16_t color, void (*set_pixel)(int, int, uint16_t), FontType font);

#endif // __FONTS_H__