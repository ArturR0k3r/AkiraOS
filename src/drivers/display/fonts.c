/* vim: set ai et ts=4 sw=4: */
// Font remade as 2D arrays for direct access: font7x10[char][row], font11x18[char][row]
#include <stdint.h>
#include "fonts.h"

// Draw a single character with font selection
void draw_char(int x, int y, char c, uint16_t color, void (*set_pixel)(int, int, uint16_t), FontType font)
{
    if (!set_pixel)
        return;

    switch (font)
    {
    case FONT_7X10:
        if (c < FONT7X10_FIRST_CHAR || c > FONT7X10_LAST_CHAR)
            return;
        {
            int idx = c - FONT7X10_FIRST_CHAR;
            for (int row = 0; row < FONT7X10_HEIGHT; row++)
            {
                uint16_t bits = font7x10[idx][row];

                // Fix: Extract bits correctly for the font data format
                // The font data appears to use the high bits, so we check from bit 15 down
                for (int col = 0; col < FONT7X10_WIDTH; col++)
                {
                    // Check bit position from high bits (15, 14, 13, etc.)
                    if (bits & (0x8000 >> col))
                    {
                        set_pixel(x + col, y + row, color);
                    }
                }
            }
        }
        break;

    case FONT_11X18:
        if (c < FONT11X18_FIRST_CHAR || c > FONT11X18_LAST_CHAR)
            return;
        {
            int idx = c - FONT11X18_FIRST_CHAR;
            for (int row = 0; row < FONT11X18_HEIGHT; row++)
            {
                uint16_t bits = font11x18[idx][row];

                // Fix: Extract bits correctly for the 11-bit wide font
                for (int col = 0; col < FONT11X18_WIDTH; col++)
                {
                    // Check bit position from high bits (15, 14, 13, etc.)
                    if (bits & (0x8000 >> col))
                    {
                        set_pixel(x + col, y + row, color);
                    }
                }
            }
        }
        break;

    default:
        break;
    }
}

void draw_string(int x, int y, const char *str, uint16_t color, void (*set_pixel)(int, int, uint16_t), FontType font)
{
    if (!str || !set_pixel)
        return;

    int cursor_x = x;
    int cursor_y = y;
    int char_width = (font == FONT_7X10) ? FONT7X10_WIDTH : FONT11X18_WIDTH;
    int char_height = (font == FONT_7X10) ? FONT7X10_HEIGHT : FONT11X18_HEIGHT;
    int tab_size = 4; // Tab size in characters

    while (*str)
    {
        if (*str == '\n')
        {
            // Newline: move to next line
            cursor_x = x;
            cursor_y += char_height + 2; // Add 2 pixels line spacing
        }
        else if (*str == '\t')
        {
            // Tab: move to next tab stop
            int chars_from_line_start = (cursor_x - x) / (char_width + 1);
            int spaces_to_next_tab = tab_size - (chars_from_line_start % tab_size);
            cursor_x += spaces_to_next_tab * (char_width + 1);
        }
        else
        {
            // Regular character
            draw_char(cursor_x, cursor_y, *str, color, set_pixel, font);
            cursor_x += char_width + 1; // Add 1 pixel spacing between characters
        }
        str++;
    }
}