/**
 * @file shell_display.c
 * @brief Shell output rendering on ILI9341 display
 * 
 * Provides a terminal-like interface for shell commands on the TFT screen.
 * Features scrolling text buffer, command history, and status bar.
 */

#include "shell_display.h"
#include "../drivers/display_ili9341.h"
#include "../drivers/fonts.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(shell_display, LOG_LEVEL_INF);

/* Configuration */
#define SHELL_DISPLAY_WIDTH     ILI9341_DISPLAY_WIDTH   // 320px
#define SHELL_DISPLAY_HEIGHT    ILI9341_DISPLAY_HEIGHT  // 240px
#define SHELL_FONT_WIDTH        7   // Font7x10 character width
#define SHELL_FONT_HEIGHT       10  // Font7x10 character height
#define SHELL_PADDING_TOP       2
#define SHELL_PADDING_LEFT      2
#define SHELL_STATUS_BAR_HEIGHT 12
#define SHELL_MAX_COLS          ((SHELL_DISPLAY_WIDTH - SHELL_PADDING_LEFT * 2) / SHELL_FONT_WIDTH)
#define SHELL_MAX_ROWS          ((SHELL_DISPLAY_HEIGHT - SHELL_STATUS_BAR_HEIGHT - SHELL_PADDING_TOP * 2) / SHELL_FONT_HEIGHT)

/* Colors */
#define SHELL_BG_COLOR          BLACK_COLOR
#define SHELL_TEXT_COLOR        GREEN_COLOR
#define SHELL_PROMPT_COLOR      CYAN_COLOR
#define SHELL_ERROR_COLOR       RED_COLOR
#define SHELL_STATUS_BG_COLOR   0x2104  // Dark gray
#define SHELL_STATUS_TEXT_COLOR WHITE_COLOR

/* Text buffer structure */
typedef struct {
    char lines[SHELL_MAX_ROWS][SHELL_MAX_COLS + 1];  // Text buffer (+1 for null terminator)
    uint16_t colors[SHELL_MAX_ROWS];                  // Line colors
    uint8_t line_count;                               // Number of lines used
    uint8_t scroll_offset;                            // Scroll position
    bool dirty;                                       // Needs redraw
} shell_text_buffer_t;

/* Shell display state */
static struct {
    shell_text_buffer_t buffer;
    char input_line[SHELL_MAX_COLS + 1];              // Current input line
    uint8_t cursor_pos;                               // Cursor position in input
    bool initialized;
    bool enabled;
    K_MUTEX_DEFINE(mutex);
} shell_display = {
    .initialized = false,
    .enabled = false,
};

/* Forward declarations */
static void render_status_bar(void);
static void render_text_buffer(void);
static void render_input_line(void);
static void scroll_buffer_up(void);
static void clear_line(uint8_t row);

/**
 * @brief Initialize shell display system
 */
int shell_display_init(void)
{
    if (shell_display.initialized) {
        return 0;
    }

    // Initialize display hardware
    int ret = ili9341_init();
    if (ret < 0) {
        LOG_ERR("Failed to initialize display: %d", ret);
        return ret;
    }

    // Clear screen
    ili9341_fill_screen(SHELL_BG_COLOR);

    // Initialize buffer
    memset(&shell_display.buffer, 0, sizeof(shell_text_buffer_t));
    memset(shell_display.input_line, 0, sizeof(shell_display.input_line));
    shell_display.cursor_pos = 0;
    shell_display.buffer.dirty = true;

    // Draw initial UI
    render_status_bar();
    
    shell_display.initialized = true;
    shell_display.enabled = true;

    LOG_INF("Shell display initialized (%dx%d, %dx%d chars)",
            SHELL_DISPLAY_WIDTH, SHELL_DISPLAY_HEIGHT,
            SHELL_MAX_COLS, SHELL_MAX_ROWS);

    return 0;
}

/**
 * @brief Enable/disable shell display output
 */
void shell_display_set_enabled(bool enabled)
{
    k_mutex_lock(&shell_display.mutex, K_FOREVER);
    shell_display.enabled = enabled;
    k_mutex_unlock(&shell_display.mutex);
}

/**
 * @brief Check if shell display is enabled
 */
bool shell_display_is_enabled(void)
{
    return shell_display.enabled && shell_display.initialized;
}

/**
 * @brief Add text line to display buffer
 */
void shell_display_print(const char *text, shell_text_type_t type)
{
    if (!shell_display_is_enabled() || !text) {
        return;
    }

    k_mutex_lock(&shell_display.mutex, K_FOREVER);

    // Determine color based on type
    uint16_t color;
    switch (type) {
        case SHELL_TEXT_ERROR:
            color = SHELL_ERROR_COLOR;
            break;
        case SHELL_TEXT_PROMPT:
            color = SHELL_PROMPT_COLOR;
            break;
        case SHELL_TEXT_NORMAL:
        default:
            color = SHELL_TEXT_COLOR;
            break;
    }

    // Word wrap long lines
    const char *p = text;
    while (*p) {
        // Scroll buffer if full
        if (shell_display.buffer.line_count >= SHELL_MAX_ROWS) {
            scroll_buffer_up();
        }

        uint8_t line_idx = shell_display.buffer.line_count;
        char *line = shell_display.buffer.lines[line_idx];
        
        // Copy up to max columns
        size_t len = 0;
        while (*p && *p != '\n' && len < SHELL_MAX_COLS) {
            line[len++] = *p++;
        }
        line[len] = '\0';

        shell_display.buffer.colors[line_idx] = color;
        shell_display.buffer.line_count++;
        shell_display.buffer.dirty = true;

        // Skip newline
        if (*p == '\n') {
            p++;
        }

        // Break if no more text
        if (!*p) {
            break;
        }
    }

    k_mutex_unlock(&shell_display.mutex);

    // Trigger redraw
    shell_display_refresh();
}

/**
 * @brief Printf-style text output
 */
void shell_display_printf(shell_text_type_t type, const char *fmt, ...)
{
    if (!shell_display_is_enabled()) {
        return;
    }

    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    shell_display_print(buffer, type);
}

/**
 * @brief Clear display buffer
 */
void shell_display_clear(void)
{
    if (!shell_display_is_enabled()) {
        return;
    }

    k_mutex_lock(&shell_display.mutex, K_FOREVER);

    memset(&shell_display.buffer, 0, sizeof(shell_text_buffer_t));
    shell_display.buffer.dirty = true;

    k_mutex_unlock(&shell_display.mutex);

    // Clear screen
    ili9341_fill_rect(0, SHELL_STATUS_BAR_HEIGHT, 
                      SHELL_DISPLAY_WIDTH, 
                      SHELL_DISPLAY_HEIGHT - SHELL_STATUS_BAR_HEIGHT,
                      SHELL_BG_COLOR);
}

/**
 * @brief Refresh display (redraw if dirty)
 */
void shell_display_refresh(void)
{
    if (!shell_display_is_enabled()) {
        return;
    }

    k_mutex_lock(&shell_display.mutex, K_FOREVER);

    if (shell_display.buffer.dirty) {
        render_text_buffer();
        render_input_line();
        shell_display.buffer.dirty = false;
    }

    k_mutex_unlock(&shell_display.mutex);
}

/**
 * @brief Update input line display
 */
void shell_display_set_input(const char *text, uint8_t cursor_pos)
{
    if (!shell_display_is_enabled() || !text) {
        return;
    }

    k_mutex_lock(&shell_display.mutex, K_FOREVER);

    strncpy(shell_display.input_line, text, SHELL_MAX_COLS);
    shell_display.input_line[SHELL_MAX_COLS] = '\0';
    shell_display.cursor_pos = cursor_pos;

    k_mutex_unlock(&shell_display.mutex);

    render_input_line();
}

/**
 * @brief Scroll text buffer up by one line
 */
static void scroll_buffer_up(void)
{
    // Shift all lines up
    for (int i = 0; i < SHELL_MAX_ROWS - 1; i++) {
        memcpy(shell_display.buffer.lines[i], 
               shell_display.buffer.lines[i + 1], 
               SHELL_MAX_COLS + 1);
        shell_display.buffer.colors[i] = shell_display.buffer.colors[i + 1];
    }

    // Clear last line
    memset(shell_display.buffer.lines[SHELL_MAX_ROWS - 1], 0, SHELL_MAX_COLS + 1);
    shell_display.buffer.line_count = SHELL_MAX_ROWS - 1;
}

/**
 * @brief Render status bar at top of screen
 */
static void render_status_bar(void)
{
    // Background
    ili9341_fill_rect(0, 0, SHELL_DISPLAY_WIDTH, SHELL_STATUS_BAR_HEIGHT, 
                      SHELL_STATUS_BG_COLOR);

    // Title text
    const char *title = "AkiraOS Shell";
    ili9341_draw_string(2, 2, title, Font7x10, SHELL_STATUS_TEXT_COLOR, 
                        SHELL_STATUS_BG_COLOR);

    // System info (right side)
    uint64_t uptime_sec = k_uptime_get() / 1000;
    char info[32];
    snprintf(info, sizeof(info), "%02llu:%02llu:%02llu", 
             uptime_sec / 3600, 
             (uptime_sec % 3600) / 60, 
             uptime_sec % 60);
    
    // Right-aligned
    int info_width = strlen(info) * SHELL_FONT_WIDTH;
    ili9341_draw_string(SHELL_DISPLAY_WIDTH - info_width - 2, 2, 
                        info, Font7x10, SHELL_STATUS_TEXT_COLOR, 
                        SHELL_STATUS_BG_COLOR);
}

/**
 * @brief Render text buffer content
 */
static void render_text_buffer(void)
{
    int y_offset = SHELL_STATUS_BAR_HEIGHT + SHELL_PADDING_TOP;

    for (int i = 0; i < shell_display.buffer.line_count; i++) {
        int y = y_offset + (i * SHELL_FONT_HEIGHT);
        
        if (y + SHELL_FONT_HEIGHT > SHELL_DISPLAY_HEIGHT - SHELL_FONT_HEIGHT) {
            break;  // Don't overlap input line
        }

        // Clear line background
        ili9341_fill_rect(SHELL_PADDING_LEFT, y, 
                          SHELL_DISPLAY_WIDTH - SHELL_PADDING_LEFT * 2, 
                          SHELL_FONT_HEIGHT, 
                          SHELL_BG_COLOR);

        // Draw text
        if (shell_display.buffer.lines[i][0] != '\0') {
            ili9341_draw_string(SHELL_PADDING_LEFT, y, 
                               shell_display.buffer.lines[i], 
                               Font7x10,
                               shell_display.buffer.colors[i], 
                               SHELL_BG_COLOR);
        }
    }
}

/**
 * @brief Render input line with prompt and cursor
 */
static void render_input_line(void)
{
    int y = SHELL_DISPLAY_HEIGHT - SHELL_FONT_HEIGHT - 2;

    // Clear input line area
    ili9341_fill_rect(0, y, SHELL_DISPLAY_WIDTH, SHELL_FONT_HEIGHT + 2, 
                      SHELL_BG_COLOR);

    // Draw prompt
    const char *prompt = "$ ";
    int prompt_width = strlen(prompt) * SHELL_FONT_WIDTH;
    ili9341_draw_string(SHELL_PADDING_LEFT, y, prompt, Font7x10, 
                        SHELL_PROMPT_COLOR, SHELL_BG_COLOR);

    // Draw input text
    if (shell_display.input_line[0] != '\0') {
        ili9341_draw_string(SHELL_PADDING_LEFT + prompt_width, y, 
                           shell_display.input_line, Font7x10,
                           SHELL_TEXT_COLOR, SHELL_BG_COLOR);
    }

    // Draw cursor (blinking block)
    int cursor_x = SHELL_PADDING_LEFT + prompt_width + 
                   (shell_display.cursor_pos * SHELL_FONT_WIDTH);
    
    // Simple cursor (rectangle)
    ili9341_fill_rect(cursor_x, y, SHELL_FONT_WIDTH, SHELL_FONT_HEIGHT, 
                      SHELL_TEXT_COLOR);
}

/**
 * @brief Clear a specific line
 */
static void clear_line(uint8_t row)
{
    if (row >= SHELL_MAX_ROWS) {
        return;
    }

    int y = SHELL_STATUS_BAR_HEIGHT + SHELL_PADDING_TOP + (row * SHELL_FONT_HEIGHT);
    ili9341_fill_rect(SHELL_PADDING_LEFT, y, 
                      SHELL_DISPLAY_WIDTH - SHELL_PADDING_LEFT * 2, 
                      SHELL_FONT_HEIGHT, 
                      SHELL_BG_COLOR);
}

/**
 * @brief Periodic status bar update (call every second)
 */
void shell_display_update_status(void)
{
    if (!shell_display_is_enabled()) {
        return;
    }

    render_status_bar();
}
