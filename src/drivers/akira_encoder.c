/**
 * @file akira_encoder.c
 * @brief Rotary Encoder Driver Implementation
 */

#include "akira_encoder.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_encoder, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* GPIO Pin Definitions (update for your board)                             */
/*===========================================================================*/

/* Define encoder pins in devicetree or here */
#define ENCODER_A_NODE DT_ALIAS(encoder_a)
#define ENCODER_B_NODE DT_ALIAS(encoder_b)
#define ENCODER_BTN_NODE DT_ALIAS(encoder_btn)

#if DT_NODE_HAS_STATUS(ENCODER_A_NODE, okay)
static const struct gpio_dt_spec encoder_a = GPIO_DT_SPEC_GET(ENCODER_A_NODE, gpios);
static const struct gpio_dt_spec encoder_b = GPIO_DT_SPEC_GET(ENCODER_B_NODE, gpios);
#define ENCODER_HW_AVAILABLE 1
#else
#define ENCODER_HW_AVAILABLE 0
#endif

#if DT_NODE_HAS_STATUS(ENCODER_BTN_NODE, okay)
static const struct gpio_dt_spec encoder_btn = GPIO_DT_SPEC_GET(ENCODER_BTN_NODE, gpios);
#define ENCODER_BTN_HW_AVAILABLE 1
#else
#define ENCODER_BTN_HW_AVAILABLE 0
#endif

/*===========================================================================*/
/* State Machine                                                             */
/*===========================================================================*/

static struct {
    bool initialized;
    encoder_config_t config;
    
    /* Position tracking */
    int32_t position;
    int32_t last_position;
    
    /* Velocity calculation */
    int32_t velocity;
    uint32_t last_update_time;
    
    /* GPIO state */
    uint8_t last_ab_state;  /* Last A/B pin states (bits 0-1) */
    uint32_t last_change_time;
    
    /* Button state */
    bool button_pressed;
    bool last_button_state;
    uint32_t last_button_time;
    
    /* Callback */
    encoder_event_callback_t callback;
    void *callback_user_data;
    
    /* GPIO callbacks */
#if ENCODER_HW_AVAILABLE
    struct gpio_callback gpio_cb_a;
    struct gpio_callback gpio_cb_b;
#endif
#if ENCODER_BTN_HW_AVAILABLE
    struct gpio_callback gpio_cb_btn;
#endif
    
    struct k_mutex mutex;
} encoder_state;

/* Quadrature decoding lookup table */
/* Index: (old_AB << 2) | new_AB, Value: direction */
static const int8_t quadrature_table[16] = {
    0,  -1,  1,  0,  /* 00 -> 00, 01, 10, 11 */
    1,   0,  0, -1,  /* 01 -> 00, 01, 10, 11 */
   -1,   0,  0,  1,  /* 10 -> 00, 01, 10, 11 */
    0,   1, -1,  0   /* 11 -> 00, 01, 10, 11 */
};

/*===========================================================================*/
/* Helper Functions                                                          */
/*===========================================================================*/

static void encoder_fire_event(const encoder_event_t *event)
{
    if (encoder_state.callback) {
        encoder_state.callback(event, encoder_state.callback_user_data);
    }
}

static void encoder_update_velocity(void)
{
    uint32_t now = k_uptime_get_32();
    uint32_t dt = now - encoder_state.last_update_time;
    
    if (dt > 0) {
        int32_t delta = encoder_state.position - encoder_state.last_position;
        encoder_state.velocity = (delta * 1000) / dt; /* steps/sec */
        encoder_state.last_position = encoder_state.position;
        encoder_state.last_update_time = now;
    }
}

/*===========================================================================*/
/* GPIO Interrupt Handlers                                                   */
/*===========================================================================*/

#if ENCODER_HW_AVAILABLE

static void encoder_gpio_callback(const struct device *dev, 
                                   struct gpio_callback *cb, 
                                   uint32_t pins)
{
    uint32_t now = k_uptime_get_32();
    
    /* Debouncing */
    if ((now - encoder_state.last_change_time) < encoder_state.config.debounce_ms) {
        return;
    }
    
    /* Read current A/B state */
    int a = gpio_pin_get_dt(&encoder_a);
    int b = gpio_pin_get_dt(&encoder_b);
    uint8_t ab_state = (a << 1) | b;
    
    /* Decode quadrature */
    uint8_t index = (encoder_state.last_ab_state << 2) | ab_state;
    int8_t direction = quadrature_table[index];
    
    if (direction != 0) {
        /* Apply direction inversion if configured */
        if (encoder_state.config.invert_direction) {
            direction = -direction;
        }
        
        /* Update position */
        k_mutex_lock(&encoder_state.mutex, K_FOREVER);
        encoder_state.position += direction;
        
        /* Update velocity */
        encoder_update_velocity();
        
        /* Fire event every N steps (detent scaling) */
        if ((encoder_state.position % encoder_state.config.steps_per_detent) == 0) {
            encoder_event_t event = {
                .type = ENCODER_EVENT_ROTATE,
                .data.rotation = {
                    .direction = (direction > 0) ? ENCODER_DIR_CW : ENCODER_DIR_CCW,
                    .position = encoder_state.position / encoder_state.config.steps_per_detent,
                    .delta = direction,
                    .velocity = encoder_state.velocity
                },
                .timestamp = now
            };
            
            encoder_fire_event(&event);
        }
        
        k_mutex_unlock(&encoder_state.mutex);
        
        encoder_state.last_change_time = now;
    }
    
    encoder_state.last_ab_state = ab_state;
}

#endif /* ENCODER_HW_AVAILABLE */

#if ENCODER_BTN_HW_AVAILABLE

static void encoder_button_callback(const struct device *dev,
                                     struct gpio_callback *cb,
                                     uint32_t pins)
{
    uint32_t now = k_uptime_get_32();
    
    /* Debouncing */
    if ((now - encoder_state.last_button_time) < encoder_state.config.debounce_ms) {
        return;
    }
    
    /* Read button state (active low) */
    bool pressed = !gpio_pin_get_dt(&encoder_btn);
    
    if (pressed != encoder_state.last_button_state) {
        k_mutex_lock(&encoder_state.mutex, K_FOREVER);
        encoder_state.button_pressed = pressed;
        encoder_state.last_button_state = pressed;
        k_mutex_unlock(&encoder_state.mutex);
        
        encoder_event_t event = {
            .type = ENCODER_EVENT_BUTTON,
            .data.button = {
                .pressed = pressed
            },
            .timestamp = now
        };
        
        encoder_fire_event(&event);
        
        encoder_state.last_button_time = now;
        
        LOG_DBG("Encoder button: %s", pressed ? "pressed" : "released");
    }
}

#endif /* ENCODER_BTN_HW_AVAILABLE */

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int akira_encoder_init(const encoder_config_t *config)
{
    if (encoder_state.initialized) {
        return -EALREADY;
    }
    
    LOG_INF("Initializing rotary encoder driver");
    
    /* Apply default config */
    if (config) {
        encoder_state.config = *config;
    } else {
        encoder_state.config.debounce_ms = 1;
        encoder_state.config.invert_direction = false;
        encoder_state.config.steps_per_detent = 4;  /* Standard for most encoders */
        encoder_state.config.has_button = ENCODER_BTN_HW_AVAILABLE;
    }
    
    /* Initialize state */
    k_mutex_init(&encoder_state.mutex);
    encoder_state.position = 0;
    encoder_state.last_position = 0;
    encoder_state.velocity = 0;
    encoder_state.last_update_time = k_uptime_get_32();
    encoder_state.last_ab_state = 0;
    encoder_state.button_pressed = false;
    encoder_state.last_button_state = false;
    
#if ENCODER_HW_AVAILABLE
    /* Configure encoder A pin */
    if (!gpio_is_ready_dt(&encoder_a)) {
        LOG_ERR("Encoder A GPIO not ready");
        return -ENODEV;
    }
    
    gpio_pin_configure_dt(&encoder_a, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_interrupt_configure_dt(&encoder_a, GPIO_INT_EDGE_BOTH);
    
    /* Configure encoder B pin */
    if (!gpio_is_ready_dt(&encoder_b)) {
        LOG_ERR("Encoder B GPIO not ready");
        return -ENODEV;
    }
    
    gpio_pin_configure_dt(&encoder_b, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_interrupt_configure_dt(&encoder_b, GPIO_INT_EDGE_BOTH);
    
    /* Setup interrupt callbacks */
    gpio_init_callback(&encoder_state.gpio_cb_a, encoder_gpio_callback,
                      BIT(encoder_a.pin));
    gpio_add_callback(encoder_a.port, &encoder_state.gpio_cb_a);
    
    gpio_init_callback(&encoder_state.gpio_cb_b, encoder_gpio_callback,
                      BIT(encoder_b.pin));
    gpio_add_callback(encoder_b.port, &encoder_state.gpio_cb_b);
    
    /* Read initial state */
    int a = gpio_pin_get_dt(&encoder_a);
    int b = gpio_pin_get_dt(&encoder_b);
    encoder_state.last_ab_state = (a << 1) | b;
    
    LOG_INF("Encoder A/B configured (initial state: %d%d)", a, b);
#else
    LOG_WRN("Encoder hardware not available (no devicetree nodes)");
#endif

#if ENCODER_BTN_HW_AVAILABLE
    if (encoder_state.config.has_button) {
        if (!gpio_is_ready_dt(&encoder_btn)) {
            LOG_ERR("Encoder button GPIO not ready");
            return -ENODEV;
        }
        
        gpio_pin_configure_dt(&encoder_btn, GPIO_INPUT | GPIO_PULL_UP);
        gpio_pin_interrupt_configure_dt(&encoder_btn, GPIO_INT_EDGE_BOTH);
        
        gpio_init_callback(&encoder_state.gpio_cb_btn, encoder_button_callback,
                          BIT(encoder_btn.pin));
        gpio_add_callback(encoder_btn.port, &encoder_state.gpio_cb_btn);
        
        LOG_INF("Encoder button configured");
    }
#endif
    
    encoder_state.initialized = true;
    LOG_INF("Encoder driver initialized (steps_per_detent=%d)", 
            encoder_state.config.steps_per_detent);
    
    return 0;
}

int akira_encoder_deinit(void)
{
    if (!encoder_state.initialized) {
        return -EALREADY;
    }
    
#if ENCODER_HW_AVAILABLE
    gpio_remove_callback(encoder_a.port, &encoder_state.gpio_cb_a);
    gpio_remove_callback(encoder_b.port, &encoder_state.gpio_cb_b);
#endif

#if ENCODER_BTN_HW_AVAILABLE
    if (encoder_state.config.has_button) {
        gpio_remove_callback(encoder_btn.port, &encoder_state.gpio_cb_btn);
    }
#endif
    
    encoder_state.initialized = false;
    return 0;
}

int akira_encoder_register_callback(encoder_event_callback_t callback, void *user_data)
{
    if (!encoder_state.initialized) {
        return -ENODEV;
    }
    
    k_mutex_lock(&encoder_state.mutex, K_FOREVER);
    encoder_state.callback = callback;
    encoder_state.callback_user_data = user_data;
    k_mutex_unlock(&encoder_state.mutex);
    
    return 0;
}

int32_t akira_encoder_get_position(void)
{
    if (!encoder_state.initialized) {
        return 0;
    }
    
    k_mutex_lock(&encoder_state.mutex, K_FOREVER);
    int32_t pos = encoder_state.position / encoder_state.config.steps_per_detent;
    k_mutex_unlock(&encoder_state.mutex);
    
    return pos;
}

int akira_encoder_set_position(int32_t position)
{
    if (!encoder_state.initialized) {
        return -ENODEV;
    }
    
    k_mutex_lock(&encoder_state.mutex, K_FOREVER);
    encoder_state.position = position * encoder_state.config.steps_per_detent;
    encoder_state.last_position = encoder_state.position;
    k_mutex_unlock(&encoder_state.mutex);
    
    return 0;
}

int akira_encoder_reset(void)
{
    return akira_encoder_set_position(0);
}

int32_t akira_encoder_get_velocity(void)
{
    if (!encoder_state.initialized) {
        return 0;
    }
    
    k_mutex_lock(&encoder_state.mutex, K_FOREVER);
    int32_t vel = encoder_state.velocity;
    k_mutex_unlock(&encoder_state.mutex);
    
    return vel;
}

bool akira_encoder_button_pressed(void)
{
    if (!encoder_state.initialized || !encoder_state.config.has_button) {
        return false;
    }
    
    k_mutex_lock(&encoder_state.mutex, K_FOREVER);
    bool pressed = encoder_state.button_pressed;
    k_mutex_unlock(&encoder_state.mutex);
    
    return pressed;
}
