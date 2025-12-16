/**
 * @file hardware_manager.c
 * @brief Hardware Subsystem Manager Implementation
 */

#include "hardware_manager.h"
#include "event_bus.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

#include "../akira/hal/hal.h"
#include "../drivers/driver_registry.h"

#ifdef CONFIG_AKIRA_DISPLAY
#include "../ui/display/display_manager.h"
#endif

#ifdef CONFIG_AKIRA_UI_LVGL
#include "../ui/lvgl/ui_manager.h"
#endif

LOG_MODULE_REGISTER(hw_manager, CONFIG_AKIRA_LOG_LEVEL);

static bool hw_ready = false;

#ifdef CONFIG_GPIO
/* Button configuration from DTS */
#if DT_NODE_EXISTS(DT_ALIAS(sw0))
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button_cb_data;

static void button_pressed_callback(const struct device *dev, 
                                   struct gpio_callback *cb, 
                                   uint32_t pins)
{
    system_event_t event = {
        .type = EVENT_BUTTON_PRESSED,
        .timestamp = k_uptime_get(),
        .data.button = {
            .button_id = 0,
            .button_mask = pins
        }
    };
    
    event_bus_publish(&event);
    LOG_DBG("Button pressed: 0x%08x", pins);
}

static int init_button(void)
{
    int ret;
    
    if (!gpio_is_ready_dt(&button)) {
        LOG_ERR("Button GPIO not ready");
        return -ENODEV;
    }
    
    ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure button pin: %d", ret);
        return ret;
    }
    
    ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure button interrupt: %d", ret);
        return ret;
    }
    
    gpio_init_callback(&button_cb_data, button_pressed_callback, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);
    
    LOG_INF("Button initialized (pin %d)", button.pin);
    return 0;
}
#else
static int init_button(void)
{
    LOG_DBG("No button configured in device tree");
    return 0;
}
#endif /* DT_NODE_EXISTS(DT_ALIAS(sw0)) */
#else
static int init_button(void)
{
    LOG_DBG("GPIO not configured");
    return 0;
}
#endif /* CONFIG_GPIO */

int hardware_manager_init(void)
{
    int ret;
    
    LOG_INF("Initializing hardware manager");
    
    /* Initialize HAL */
    ret = hal_init();
    if (ret < 0) {
        LOG_ERR("HAL initialization failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ HAL initialized");
    
    /* Initialize driver registry */
    ret = driver_registry_init();
    if (ret < 0) {
        LOG_ERR("Driver registry initialization failed: %d", ret);
        return ret;
    }
    LOG_INF("✅ Driver registry initialized");
    
    /* Initialize button handler */
    ret = init_button();
    if (ret < 0) {
        LOG_WRN("Button initialization failed: %d (non-critical)", ret);
        /* Non-critical, continue */
    }
    
#ifdef CONFIG_AKIRA_DISPLAY
    /* Initialize display */
    ret = display_manager_init();
    if (ret < 0) {
        LOG_WRN("Display initialization failed: %d", ret);
        /* Non-critical, continue */
    } else {
        LOG_INF("✅ Display initialized");
        
        system_event_t event = {
            .type = EVENT_DISPLAY_READY,
            .timestamp = k_uptime_get()
        };
        event_bus_publish(&event);
    }
#endif

#ifdef CONFIG_AKIRA_UI_LVGL
    /* Initialize LVGL UI */
    ret = ui_manager_init();
    if (ret < 0) {
        LOG_WRN("UI manager initialization failed: %d", ret);
    } else {
        LOG_INF("✅ LVGL UI initialized");
    }
#endif
    
    hw_ready = true;
    LOG_INF("✅ Hardware manager ready");
    
    return 0;
}

bool hardware_manager_is_ready(void)
{
    return hw_ready;
}
