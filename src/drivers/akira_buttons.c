/**
 * @file akira_buttons.c
 * @brief Direct GPIO Button Driver for Akira Board
 *
 * Provides direct access to button states using Zephyr GPIO API.
 * Supports all gaming buttons (ON/OFF, SETTINGS, UP, DOWN, LEFT, RIGHT, A, B, X, Y).
 */

#include "akira_hal.h"
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_buttons, LOG_LEVEL_INF);

// Example GPIO pin mapping (update for your board)
#define BTN_ONOFF_PIN 0
#define BTN_SETTINGS_PIN 1
#define BTN_UP_PIN 2
#define BTN_DOWN_PIN 3
#define BTN_LEFT_PIN 4
#define BTN_RIGHT_PIN 5
#define BTN_A_PIN 6
#define BTN_B_PIN 7
#define BTN_X_PIN 8
#define BTN_Y_PIN 9

static const struct device *button_gpio_dev;

int akira_buttons_init(void)
{
    button_gpio_dev = device_get_binding("GPIO_0");
    if (!button_gpio_dev)
    {
        LOG_ERR("Failed to bind button GPIO device");
        return -ENODEV;
    }
    // Configure pins as input
    gpio_pin_configure(button_gpio_dev, BTN_ONOFF_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(button_gpio_dev, BTN_SETTINGS_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(button_gpio_dev, BTN_UP_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(button_gpio_dev, BTN_DOWN_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(button_gpio_dev, BTN_LEFT_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(button_gpio_dev, BTN_RIGHT_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(button_gpio_dev, BTN_A_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(button_gpio_dev, BTN_B_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(button_gpio_dev, BTN_X_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(button_gpio_dev, BTN_Y_PIN, GPIO_INPUT | GPIO_PULL_UP);
    return 0;
}

uint16_t akira_buttons_get_state(void)
{
    uint16_t state = 0;
    if (!button_gpio_dev)
        return 0;
    state |= !gpio_pin_get(button_gpio_dev, BTN_ONOFF_PIN) ? BTN_ONOFF : 0;
    state |= !gpio_pin_get(button_gpio_dev, BTN_SETTINGS_PIN) ? BTN_SETTINGS : 0;
    state |= !gpio_pin_get(button_gpio_dev, BTN_UP_PIN) ? BTN_UP : 0;
    state |= !gpio_pin_get(button_gpio_dev, BTN_DOWN_PIN) ? BTN_DOWN : 0;
    state |= !gpio_pin_get(button_gpio_dev, BTN_LEFT_PIN) ? BTN_LEFT : 0;
    state |= !gpio_pin_get(button_gpio_dev, BTN_RIGHT_PIN) ? BTN_RIGHT : 0;
    state |= !gpio_pin_get(button_gpio_dev, BTN_A_PIN) ? BTN_A : 0;
    state |= !gpio_pin_get(button_gpio_dev, BTN_B_PIN) ? BTN_B : 0;
    state |= !gpio_pin_get(button_gpio_dev, BTN_X_PIN) ? BTN_X : 0;
    state |= !gpio_pin_get(button_gpio_dev, BTN_Y_PIN) ? BTN_Y : 0;
    return state;
}
