/**
 * @file akira_encoder.h
 * @brief Rotary Encoder Driver for AkiraOS
 *
 * Provides quadrature encoder decoding with:
 * - Direction detection (CW/CCW)
 * - Position tracking
 * - Velocity calculation
 * - Event callbacks
 * - Debouncing
 */

#ifndef AKIRA_ENCODER_H
#define AKIRA_ENCODER_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Encoder direction
 */
typedef enum {
    ENCODER_DIR_NONE = 0,
    ENCODER_DIR_CW = 1,   /* Clockwise */
    ENCODER_DIR_CCW = -1  /* Counter-clockwise */
} encoder_direction_t;

/**
 * Encoder event types
 */
typedef enum {
    ENCODER_EVENT_ROTATE,    /* Rotation detected */
    ENCODER_EVENT_BUTTON,    /* Button press/release (if encoder has button) */
} encoder_event_type_t;

/**
 * Encoder event structure
 */
typedef struct {
    encoder_event_type_t type;
    union {
        struct {
            encoder_direction_t direction;
            int32_t position;
            int32_t delta;       /* Position change since last event */
            int32_t velocity;    /* Steps per second */
        } rotation;
        struct {
            bool pressed;
        } button;
    } data;
    uint32_t timestamp;         /* Milliseconds since boot */
} encoder_event_t;

/**
 * Encoder event callback
 * @param event Encoder event data
 * @param user_data User-provided context
 */
typedef void (*encoder_event_callback_t)(const encoder_event_t *event, void *user_data);

/**
 * Encoder configuration
 */
typedef struct {
    uint32_t debounce_ms;        /* Debounce time in milliseconds (default: 1ms) */
    bool invert_direction;       /* Invert rotation direction */
    int32_t steps_per_detent;    /* Steps per physical detent (1 or 2 or 4) */
    bool has_button;             /* Encoder has integrated button */
} encoder_config_t;

/**
 * Initialize encoder driver
 * @param config Encoder configuration (NULL for defaults)
 * @return 0 on success, negative errno on error
 */
int akira_encoder_init(const encoder_config_t *config);

/**
 * Deinitialize encoder driver
 * @return 0 on success, negative errno on error
 */
int akira_encoder_deinit(void);

/**
 * Register event callback
 * @param callback Callback function
 * @param user_data User context passed to callback
 * @return 0 on success, negative errno on error
 */
int akira_encoder_register_callback(encoder_event_callback_t callback, void *user_data);

/**
 * Get current encoder position
 * @return Current position in steps
 */
int32_t akira_encoder_get_position(void);

/**
 * Set encoder position (for calibration)
 * @param position New position value
 * @return 0 on success, negative errno on error
 */
int akira_encoder_set_position(int32_t position);

/**
 * Reset encoder position to zero
 * @return 0 on success, negative errno on error
 */
int akira_encoder_reset(void);

/**
 * Get encoder velocity (steps per second)
 * @return Velocity in steps/sec (positive=CW, negative=CCW, 0=stopped)
 */
int32_t akira_encoder_get_velocity(void);

/**
 * Check if encoder button is pressed (if has_button=true)
 * @return true if pressed, false otherwise
 */
bool akira_encoder_button_pressed(void);

#endif /* AKIRA_ENCODER_H */
