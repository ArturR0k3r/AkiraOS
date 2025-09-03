#ifndef USER_LED_H
#define USER_LED_H

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

// Define LED pin directly
#define USER_LED_PIN 2

struct user_led
{
    gpio_pin_t pin;
};

int user_led_init(void);
void user_led_on(void);
void user_led_off(void);
void user_led_toggle(void);

#endif // USER_LED_H