/**
 * @file akira_buttons.c
 * @brief Enhanced Button Driver for AkiraOS
 *
 * Provides:
 * - GPIO interrupt-based button detection
 * - Software debouncing (configurable)
 * - Long press detection
 * - Key repeat for held buttons
 * - Event callbacks
 */

#include "akira_buttons.h"
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_buttons, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* GPIO Pin Definitions                                                      */
/*===========================================================================*/

/* Define button pins - update based on your board's devicetree */
#define BUTTON_GPIO_NODE DT_NODELABEL(gpio0)

/* Pin mappings (update for XIAO nRF52840 or your board) */
static const uint8_t button_pins[BUTTON_COUNT] = {
    0,  /* BUTTON_ONOFF */
    1,  /* BUTTON_SETTINGS */
    2,  /* BUTTON_UP */
    3,  /* BUTTON_DOWN */
    4,  /* BUTTON_LEFT */
    5,  /* BUTTON_RIGHT */
    6,  /* BUTTON_A */
    7,  /* BUTTON_B */
    8,  /* BUTTON_X */
    9   /* BUTTON_Y */
};

/*===========================================================================*/
/* State Management                                                          */
/*===========================================================================*/

typedef struct {
    bool current_state;      /* Current physical state (debounced) */
    bool last_state;         /* Previous state */
    uint32_t press_time;     /* Time when button was pressed */
    uint32_t last_change;    /* Last debounce timestamp */
    bool long_press_fired;   /* Long press event already fired */
    uint32_t last_repeat;    /* Last repeat event timestamp */
} button_state_t;

static struct {
    bool initialized;
    button_config_t config;
    
    const struct device *gpio_dev;
    button_state_t buttons[BUTTON_COUNT];
    
    button_event_callback_t callback;
    void *callback_user_data;
    
    struct gpio_callback gpio_callbacks[BUTTON_COUNT];
    struct k_work_delayable monitor_work;
    
    struct k_mutex mutex;
} button_state;

/*===========================================================================*/
/* Helper Functions                                                          */
/*===========================================================================*/

static void button_fire_event(const button_event_t *event)
{
    if (button_state.callback) {
        button_state.callback(event, button_state.callback_user_data);
    }
}

static void button_process_state_change(button_id_t id, bool pressed, uint32_t now)
{
    button_state_t *btn = &button_state.buttons[id];
    
    if (pressed) {
        /* Button pressed */
        btn->press_time = now;
        btn->long_press_fired = false;
        btn->last_repeat = 0;
        
        button_event_t event = {
            .button = id,
            .type = BUTTON_EVENT_PRESS,
            .duration_ms = 0,
            .timestamp = now
        };
        button_fire_event(&event);
        
        LOG_DBG("Button %d pressed", id);
        
    } else {
        /* Button released */
        uint32_t duration = now - btn->press_time;
        
        button_event_t event = {
            .button = id,
            .type = BUTTON_EVENT_RELEASE,
            .duration_ms = duration,
            .timestamp = now
        };
        button_fire_event(&event);
        
        /* Fire CLICK event if it was a short press (not long press) */
        if (!btn->long_press_fired && duration < button_state.config.long_press_ms) {
            event.type = BUTTON_EVENT_CLICK;
            button_fire_event(&event);
        }
        
        LOG_DBG("Button %d released (held %u ms)", id, duration);
    }
}

/*===========================================================================*/
/* GPIO Interrupt Handler                                                    */
/*===========================================================================*/

static void button_gpio_callback(const struct device *dev,
                                  struct gpio_callback *cb,
                                  uint32_t pins)
{
    uint32_t now = k_uptime_get_32();
    
    /* Find which button triggered */
    for (button_id_t id = 0; id < BUTTON_COUNT; id++) {
        if (pins & BIT(button_pins[id])) {
            button_state_t *btn = &button_state.buttons[id];
            
            /* Debouncing */
            if ((now - btn->last_change) < button_state.config.debounce_ms) {
                continue;
            }
            
            /* Read current state (active low) */
            int pin_state = gpio_pin_get(button_state.gpio_dev, button_pins[id]);
            bool pressed = !pin_state;
            
            /* State changed? */
            if (pressed != btn->current_state) {
                k_mutex_lock(&button_state.mutex, K_FOREVER);
                btn->current_state = pressed;
                btn->last_change = now;
                
                button_process_state_change(id, pressed, now);
                k_mutex_unlock(&button_state.mutex);
            }
        }
    }
}

/*===========================================================================*/
/* Background Monitor (for long press and repeat)                           */
/*===========================================================================*/

static void button_monitor_work_handler(struct k_work *work)
{
    uint32_t now = k_uptime_get_32();
    
    k_mutex_lock(&button_state.mutex, K_FOREVER);
    
    for (button_id_t id = 0; id < BUTTON_COUNT; id++) {
        button_state_t *btn = &button_state.buttons[id];
        
        if (btn->current_state) {  /* Button is pressed */
            uint32_t hold_time = now - btn->press_time;
            
            /* Long press detection */
            if (!btn->long_press_fired && hold_time >= button_state.config.long_press_ms) {
                btn->long_press_fired = true;
                
                button_event_t event = {
                    .button = id,
                    .type = BUTTON_EVENT_HOLD,
                    .duration_ms = hold_time,
                    .timestamp = now
                };
                button_fire_event(&event);
                
                LOG_DBG("Button %d long press", id);
            }
            
            /* Key repeat */
            if (button_state.config.repeat_enabled && btn->long_press_fired) {
                uint32_t repeat_delay = (btn->last_repeat == 0) ? 
                    button_state.config.repeat_delay_ms : 
                    button_state.config.repeat_interval_ms;
                
                if ((btn->last_repeat == 0 && hold_time >= button_state.config.long_press_ms + repeat_delay) ||
                    (btn->last_repeat > 0 && (now - btn->last_repeat) >= button_state.config.repeat_interval_ms)) {
                    
                    btn->last_repeat = now;
                    
                    /* Fire press event for repeat */
                    button_event_t event = {
                        .button = id,
                        .type = BUTTON_EVENT_PRESS,
                        .duration_ms = hold_time,
                        .timestamp = now
                    };
                    button_fire_event(&event);
                }
            }
        }
    }
    
    k_mutex_unlock(&button_state.mutex);
    
    /* Schedule next check */
    k_work_schedule(&button_state.monitor_work, K_MSEC(50));
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

int akira_buttons_init(const button_config_t *config)
{
    if (button_state.initialized) {
        return -EALREADY;
    }
    
    LOG_INF("Initializing enhanced button driver");
    
    /* Apply configuration */
    if (config) {
        button_state.config = *config;
    } else {
        /* Default configuration */
        button_state.config.debounce_ms = 20;
        button_state.config.long_press_ms = 1000;
        button_state.config.repeat_enabled = false;
        button_state.config.repeat_delay_ms = 500;
        button_state.config.repeat_interval_ms = 100;
    }
    
    /* Initialize mutex */
    k_mutex_init(&button_state.mutex);
    
    /* Get GPIO device */
    button_state.gpio_dev = DEVICE_DT_GET(BUTTON_GPIO_NODE);
    if (!device_is_ready(button_state.gpio_dev)) {
        LOG_ERR("Button GPIO device not ready");
        return -ENODEV;
    }
    
    /* Configure all button pins */
    for (button_id_t id = 0; id < BUTTON_COUNT; id++) {
        uint8_t pin = button_pins[id];
        
        /* Configure as input with pull-up (active low) */
        int ret = gpio_pin_configure(button_state.gpio_dev, pin, 
                                     GPIO_INPUT | GPIO_PULL_UP);
        if (ret < 0) {
            LOG_ERR("Failed to configure button %d pin %d", id, pin);
            return ret;
        }
        
        /* Setup interrupt */
        ret = gpio_pin_interrupt_configure(button_state.gpio_dev, pin, 
                                           GPIO_INT_EDGE_BOTH);
        if (ret < 0) {
            LOG_ERR("Failed to configure interrupt for button %d", id);
            return ret;
        }
        
        /* Initialize button state */
        button_state.buttons[id].current_state = false;
        button_state.buttons[id].last_state = false;
        button_state.buttons[id].press_time = 0;
        button_state.buttons[id].last_change = 0;
        button_state.buttons[id].long_press_fired = false;
        button_state.buttons[id].last_repeat = 0;
        
        /* Setup GPIO callback */
        gpio_init_callback(&button_state.gpio_callbacks[id], 
                          button_gpio_callback, BIT(pin));
        gpio_add_callback(button_state.gpio_dev, &button_state.gpio_callbacks[id]);
    }
    
    /* Initialize background monitor work */
    k_work_init_delayable(&button_state.monitor_work, button_monitor_work_handler);
    k_work_schedule(&button_state.monitor_work, K_MSEC(50));
    
    button_state.initialized = true;
    
    LOG_INF("Button driver initialized (debounce=%ums, long_press=%ums)",
            button_state.config.debounce_ms, button_state.config.long_press_ms);
    
    return 0;
}

int akira_buttons_deinit(void)
{
    if (!button_state.initialized) {
        return -EALREADY;
    }
    
    /* Cancel background work */
    k_work_cancel_delayable(&button_state.monitor_work);
    
    /* Remove GPIO callbacks */
    for (button_id_t id = 0; id < BUTTON_COUNT; id++) {
        gpio_remove_callback(button_state.gpio_dev, &button_state.gpio_callbacks[id]);
    }
    
    button_state.initialized = false;
    return 0;
}

int akira_buttons_register_callback(button_event_callback_t callback, void *user_data)
{
    if (!button_state.initialized) {
        return -ENODEV;
    }
    
    k_mutex_lock(&button_state.mutex, K_FOREVER);
    button_state.callback = callback;
    button_state.callback_user_data = user_data;
    k_mutex_unlock(&button_state.mutex);
    
    return 0;
}

uint16_t akira_buttons_get_state(void)
{
    if (!button_state.initialized) {
        return 0;
    }
    
    uint16_t state = 0;
    
    k_mutex_lock(&button_state.mutex, K_FOREVER);
    for (button_id_t id = 0; id < BUTTON_COUNT; id++) {
        if (button_state.buttons[id].current_state) {
            state |= (1 << id);
        }
    }
    k_mutex_unlock(&button_state.mutex);
    
    return state;
}

bool akira_buttons_is_pressed(button_id_t button)
{
    if (!button_state.initialized || button >= BUTTON_COUNT) {
        return false;
    }
    
    k_mutex_lock(&button_state.mutex, K_FOREVER);
    bool pressed = button_state.buttons[button].current_state;
    k_mutex_unlock(&button_state.mutex);
    
    return pressed;
}

uint32_t akira_buttons_get_hold_time(button_id_t button)
{
    if (!button_state.initialized || button >= BUTTON_COUNT) {
        return 0;
    }
    
    k_mutex_lock(&button_state.mutex, K_FOREVER);
    uint32_t hold_time = 0;
    
    if (button_state.buttons[button].current_state) {
        hold_time = k_uptime_get_32() - button_state.buttons[button].press_time;
    }
    
    k_mutex_unlock(&button_state.mutex);
    
    return hold_time;
}
