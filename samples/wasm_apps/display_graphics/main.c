/**
 * @file main.c
 * @brief Display Graphics Demo - Shows display API usage
 * 
 * Demonstrates:
 * - Drawing primitives (pixels, rectangles, text)
 * - Color animations
 * - Simple game-like graphics
 */

#include "akira_api.h"

/* Display constants */
#define DISPLAY_WIDTH  320
#define DISPLAY_HEIGHT 240

/* Colors (RGB565) */
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F

/* Game state */
typedef struct {
    int16_t x, y;
    int16_t dx, dy;
    uint16_t color;
} Ball;

static Ball ball = {
    .x = DISPLAY_WIDTH / 2,
    .y = DISPLAY_HEIGHT / 2,
    .dx = 2,
    .dy = 2,
    .color = COLOR_RED
};

static uint32_t score = 0;

/**
 * @brief Draw bouncing ball animation
 */
void draw_bouncing_ball(void)
{
    const uint8_t BALL_SIZE = 8;
    
    /* Clear previous ball position */
    akira_display_rect(ball.x - BALL_SIZE/2, ball.y - BALL_SIZE/2, 
                       BALL_SIZE, BALL_SIZE, COLOR_BLACK);
    
    /* Update position */
    ball.x += ball.dx;
    ball.y += ball.dy;
    
    /* Bounce off edges */
    if (ball.x <= BALL_SIZE/2 || ball.x >= DISPLAY_WIDTH - BALL_SIZE/2) {
        ball.dx = -ball.dx;
        ball.color = (ball.color + 0x0841) & 0xFFFF;  // Change color
        score++;
    }
    if (ball.y <= BALL_SIZE/2 || ball.y >= DISPLAY_HEIGHT - BALL_SIZE/2) {
        ball.dy = -ball.dy;
        ball.color = (ball.color + 0x0841) & 0xFFFF;
        score++;
    }
    
    /* Draw ball at new position */
    akira_display_rect(ball.x - BALL_SIZE/2, ball.y - BALL_SIZE/2, 
                       BALL_SIZE, BALL_SIZE, ball.color);
}

/**
 * @brief Draw score display
 */
void draw_score(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "Score: %lu", score);
    
    /* Clear score area */
    akira_display_rect(0, 0, 120, 20, COLOR_BLACK);
    
    /* Draw score text */
    akira_display_text(5, 5, buf, COLOR_YELLOW, COLOR_BLACK, 0);
}

/**
 * @brief Draw colorful pixel pattern
 */
void draw_pixel_pattern(void)
{
    for (int y = 0; y < 50; y++) {
        for (int x = 0; x < 50; x++) {
            uint16_t color = ((x * 8) << 11) | ((y * 4) << 5) | ((x + y) & 0x1F);
            akira_display_pixel(x + 10, y + 50, color);
        }
    }
}

/**
 * @brief Draw gradient bars
 */
void draw_gradients(void)
{
    /* Red gradient */
    for (int i = 0; i < 100; i++) {
        uint16_t color = (i * 31 / 100) << 11;
        akira_display_rect(200, 50 + i, 50, 1, color);
    }
    
    /* Green gradient */
    for (int i = 0; i < 100; i++) {
        uint16_t color = (i * 63 / 100) << 5;
        akira_display_rect(260, 50 + i, 50, 1, color);
    }
}

/**
 * @brief Main entry point
 */
void _start()
{
    /* Initialize display */
    int width, height;
    akira_display_get_size(&width, &height);
    
    /* Clear screen */
    akira_display_clear(COLOR_BLACK);
    
    /* Draw title */
    akira_display_text(80, 10, "Display Demo", COLOR_CYAN, COLOR_BLACK, 1);
    
    /* Draw static patterns */
    draw_pixel_pattern();
    draw_gradients();
    
    /* Info text */
    akira_display_text(10, height - 30, "Press STOP to exit", COLOR_WHITE, COLOR_BLACK, 0);
    
    /* Flush initial frame */
    akira_display_flush();
    
    /* Animation loop */
    uint32_t frame = 0;
    while (1) {
        /* Draw bouncing ball */
        draw_bouncing_ball();
        
        /* Update score every 10 frames */
        if (frame % 10 == 0) {
            draw_score();
        }
        
        /* Flush display */
        akira_display_flush();
        
        /* Frame rate control */
        akira_system_sleep_ms(16);  // ~60 FPS
        frame++;
    }
}
