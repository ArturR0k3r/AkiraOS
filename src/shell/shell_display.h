/**
 * @file shell_display.h
 * @brief Shell output rendering on ILI9341 display
 */

#ifndef SHELL_DISPLAY_H
#define SHELL_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

/**
 * @brief Text type for color coding
 */
typedef enum {
    SHELL_TEXT_NORMAL,   // Regular output (green)
    SHELL_TEXT_ERROR,    // Error messages (red)
    SHELL_TEXT_PROMPT,   // Command prompt (cyan)
} shell_text_type_t;

/**
 * @brief Initialize shell display system
 * 
 * Sets up ILI9341 display and text buffer for shell output.
 * 
 * @return 0 on success, negative error code on failure
 */
int shell_display_init(void);

/**
 * @brief Enable or disable shell display output
 * 
 * @param enabled True to enable, false to disable
 */
void shell_display_set_enabled(bool enabled);

/**
 * @brief Check if shell display is enabled
 * 
 * @return True if enabled and initialized
 */
bool shell_display_is_enabled(void);

/**
 * @brief Print text line to shell display
 * 
 * Supports automatic word wrapping and scrolling.
 * 
 * @param text Text to display (null-terminated)
 * @param type Text type for color coding
 */
void shell_display_print(const char *text, shell_text_type_t type);

/**
 * @brief Printf-style formatted output
 * 
 * @param type Text type for color coding
 * @param fmt Printf format string
 * @param ... Variable arguments
 */
void shell_display_printf(shell_text_type_t type, const char *fmt, ...);

/**
 * @brief Clear shell display buffer
 * 
 * Clears all text and resets buffer.
 */
void shell_display_clear(void);

/**
 * @brief Refresh display (redraw if dirty)
 * 
 * Call periodically or after batch updates.
 */
void shell_display_refresh(void);

/**
 * @brief Update input line display
 * 
 * Shows current command being typed with cursor.
 * 
 * @param text Current input text
 * @param cursor_pos Cursor position (0-based)
 */
void shell_display_set_input(const char *text, uint8_t cursor_pos);

/**
 * @brief Update status bar (call every second)
 * 
 * Updates system info (uptime, etc.) in status bar.
 */
void shell_display_update_status(void);

#endif // SHELL_DISPLAY_H
