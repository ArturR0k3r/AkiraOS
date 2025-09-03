#include "user_led.h"
#include <zephyr/kernel.h>

static struct user_led led;
static const struct device *gpio_dev;

int user_led_init(void)
{
    // Get GPIO device
    gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio_dev))
    {
        return -ENODEV;
    }

    // Initialize LED structure
    led.pin = USER_LED_PIN;

    // Configure pin as output, initially off
    int ret = gpio_pin_configure(gpio_dev, led.pin, GPIO_OUTPUT);
    if (ret < 0)
    {
        return ret;
    }

    // Turn LED off initially
    gpio_pin_set(gpio_dev, led.pin, 0);

    return 0;
}

void user_led_on(void)
{
    gpio_pin_set(gpio_dev, led.pin, 1);
}

void user_led_off(void)
{
    gpio_pin_set(gpio_dev, led.pin, 0);
}

void user_led_toggle(void)
{
    gpio_pin_toggle(gpio_dev, led.pin);
}