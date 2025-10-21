/**
 * @file akira_sim.h
 * @brief Akira Console SDL2 Visual Simulator
 *
 * Provides a graphical simulation window showing the Akira Console
 * with interactive display and buttons.
 */

#ifndef AKIRA_SIM_H
#define AKIRA_SIM_H

#include <stdint.h>
#include <stdbool.h>

/* Display dimensions (ILI9341) */
#define SIM_DISPLAY_WIDTH 240
#define SIM_DISPLAY_HEIGHT 320

/* Window dimensions */
#define SIM_WINDOW_WIDTH 400
#define SIM_WINDOW_HEIGHT 600

/* Display position in window */
#define SIM_DISPLAY_X 80
#define SIM_DISPLAY_Y 60

/* Button layout */
#define SIM_BUTTON_RADIUS 25
#define SIM_NUM_BUTTONS 10

/* Button IDs matching hardware */
typedef enum
{
    AKIRA_BTN_POWER = 0, /* ON/OFF button */
    AKIRA_BTN_SETTINGS,  /* Settings button */
    AKIRA_BTN_UP,        /* D-Pad Up */
    AKIRA_BTN_DOWN,      /* D-Pad Down */
    AKIRA_BTN_LEFT,      /* D-Pad Left */
    AKIRA_BTN_RIGHT,     /* D-Pad Right */
    AKIRA_BTN_A,         /* Action button A */
    AKIRA_BTN_B,         /* Action button B */
    AKIRA_BTN_X,         /* Action button X */
    AKIRA_BTN_Y          /* Action button Y */
} akira_button_id_t;

/* Button state structure */
typedef struct
{
    int x;
    int y;
    int radius;
    bool pressed;
    akira_button_id_t id;
    const char *label;
} akira_button_t;

/**
 * @brief Initialize the SDL2 simulator window
 * @return 0 on success, negative on error
 */
int akira_sim_init(void);

/**
 * @brief Shutdown the simulator and cleanup resources
 */
void akira_sim_shutdown(void);

/**
 * @brief Update the simulator display from framebuffer
 * @param framebuffer Pointer to 240x320 RGB565 framebuffer
 */
void akira_sim_update_display(const uint16_t *framebuffer);

/**
 * @brief Process SDL events and update button states
 * @return Button state bitmask
 */
uint32_t akira_sim_process_events(void);

/**
 * @brief Check if simulator window is still open
 * @return true if window is open
 */
bool akira_sim_is_running(void);

/**
 * @brief Render the complete simulator window
 */
void akira_sim_render(void);

/**
 * @brief Get current button states
 * @return Button state bitmask (bit 0 = button 0, etc.)
 */
uint32_t akira_sim_get_button_state(void);

#endif /* AKIRA_SIM_H */
