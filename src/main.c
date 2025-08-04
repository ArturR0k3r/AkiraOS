// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_main, LOG_LEVEL_INF);

/* LED definitions */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* Display definitions */
#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
static const struct device *display_dev = DEVICE_DT_GET(DISPLAY_NODE);

int main(void)
{
    LOG_INF("=== AkiraOS v1.0.0 ===");
    LOG_INF("Cyberpunk Gaming Console");
    LOG_INF("Hardware: Akira Basic ESP32");
    LOG_INF("Build: %s %s", __DATE__, __TIME__);
    
    /* Initialize status LED */
    if (!gpio_is_ready_dt(&led)) {
        LOG_ERR("Status LED not ready");
        return -1;
    }
    
    int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure status LED");
        return -1;
    }
    
    /* Initialize display */
    if (!device_is_ready(display_dev)) {
        LOG_ERR("Display device not ready");
        return -1;
    }
    
    struct display_capabilities caps;
    display_get_capabilities(display_dev, &caps);
    LOG_INF("Display: %dx%d, %d colors", caps.x_resolution, 
            caps.y_resolution, caps.supported_pixel_formats);
    
    /* Clear display */
    display_blanking_off(display_dev);
    
    LOG_INF("AkiraOS initialization complete");
    LOG_INF("System ready for hacking...");
    
    /* Main loop */
    bool led_state = false;
    while (1) {
        gpio_pin_set_dt(&led, led_state);
        led_state = !led_state;
        
        LOG_INF("System heartbeat - LED: %s", led_state ? "ON" : "OFF");
        k_sleep(K_MSEC(1000));
    }
    
    return 0;
}