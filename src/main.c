// SPDX-License-Identifier: Apache-2.0

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(akira_main, LOG_LEVEL_DBG);

int main(void)
{
    LOG_INF("=== AkiraOS v1.0.0 ===");
    LOG_INF("Cyberpunk Gaming Console");
    LOG_INF("Hardware: Akira Basic ESP32");
    LOG_INF("Build: %s %s", __DATE__, __TIME__);

    LOG_INF("AkiraOS initialization complete");
    LOG_INF("System ready for hacking...");

    /* Main loop */
    bool led_state = false;
    while (1)
    {
        LOG_INF("System heartbeat - LED: %s", led_state ? "ON" : "OFF");
        k_msleep(500);
        bool led_state = led_state ^ led_state;
    }
    return 0;
}