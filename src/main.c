/**
 * @file main.c
 * @brief Main entry point for AkiraOS
 */

 #include <zephyr/kernel.h>
 #include <zephyr/logging/log.h>
 #include <zephyr/drivers/gpio.h>
 
 LOG_MODULE_REGISTER(akira_os, LOG_LEVEL_INF);
 
 /* Status LED */
 static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(status_led), gpios, {0});
 
 /* Define stack size for main thread */
 #define STACK_SIZE 4096
 #define PRIORITY 5
 
 /**
  * @brief Initialize hardware components
  * 
  * @return int 0 on success, negative error code otherwise
  */
 static int initialize_hardware(void)
 {
     int ret;
 
     /* Configure status LED if available */
     if (status_led.port != NULL) {
         if (!device_is_ready(status_led.port)) {
             LOG_ERR("Status LED device not ready");
             return -ENODEV;
         }
 
         ret = gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_ACTIVE);
         if (ret < 0) {
             LOG_ERR("Failed to configure status LED: %d", ret);
             return ret;
         }
 
         /* Blink LED to indicate successful hardware init */
         gpio_pin_toggle_dt(&status_led);
         k_sleep(K_MSEC(500));
         gpio_pin_toggle_dt(&status_led);
     }
 
     return 0;
 }
 
 /**
  * @brief Main function
  */
 int main(void)
 {
     LOG_INF("Starting AkiraOS");
     
     /* Initialize hardware */
     if (initialize_hardware() != 0) {
         LOG_ERR("Hardware initialization failed");
         return -1;
     }
     
     LOG_INF("Hardware initialized");
     LOG_INF("AkiraOS initialization complete");
     
     /* Main system loop */
     while (1) {
         if (status_led.port != NULL) {
             gpio_pin_toggle_dt(&status_led);
         }
         k_sleep(K_SECONDS(1));
     }
     
     return 0;
 }