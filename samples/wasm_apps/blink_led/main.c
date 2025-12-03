/**
 * @file main.c
 * @brief Blink LED - GPIO Control Example for AkiraOS
 * 
 * This example demonstrates how to:
 * - Configure GPIO pins
 * - Toggle LED output
 * - Create a simple blink pattern
 * 
 * Note: GPIO port/pin numbers depend on your hardware.
 * Modify LED_PORT and LED_PIN for your board.
 * 
 * Build with:
 *   make
 */

#include "akira_api.h"

/* LED configuration - adjust for your hardware */
#define LED_PORT    0   /* GPIO port (typically 0 for most boards) */
#define LED_PIN     2   /* GPIO pin number (e.g., GPIO2 on ESP32) */

/* Blink timing */
#define BLINK_DELAY_MS  500

/* LED state */
static int led_state = 0;

/**
 * @brief Initialize the LED GPIO
 * @return 0 on success, negative on error
 */
static int init_led(void)
{
    /* Configure pin as output */
    return ocre_gpio_configure(LED_PORT, LED_PIN, OCRE_GPIO_OUTPUT);
}

/**
 * @brief Toggle the LED state
 */
static void toggle_led(void)
{
    led_state = !led_state;
    ocre_gpio_set(LED_PORT, LED_PIN, led_state);
}

/**
 * @brief Application entry point
 */
AKIRA_APP_MAIN()
{
    /* Initialize LED GPIO */
    if (init_led() < 0) {
        /* GPIO init failed - sleep forever */
        while (1) {
            ocre_sleep(10000);
        }
    }
    
    /* Start with LED off */
    ocre_gpio_set(LED_PORT, LED_PIN, 0);
    
    /* Blink loop */
    while (1) {
        toggle_led();
        ocre_sleep(BLINK_DELAY_MS);
    }
}
