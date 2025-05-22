#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "display.h"

LOG_MODULE_DECLARE(akira_os, LOG_LEVEL_DBG);

static const struct device *display_dev;

void display_backlight_set(bool state)
{
    if (!DT_NODE_EXISTS(DT_NODELABEL(ili9341)))
    {
        LOG_ERR("ILI9341 node not found in devicetree");
        return;
    }

    if (!DT_NODE_HAS_PROP(DT_NODELABEL(ili9341), led_gpios))
    {
        LOG_ERR("ILI9341 node missing led-gpios property");
        return;
    }

    static const struct gpio_dt_spec bl_led = GPIO_DT_SPEC_GET_OR(DT_NODELABEL(ili9341), led_gpios, {0});

    if (!gpio_is_ready_dt(&bl_led))
    {
        LOG_ERR("Backlight GPIO device not ready");
        return;
    }

    int ret = gpio_pin_configure_dt(&bl_led, GPIO_OUTPUT);
    if (ret < 0)
    {
        LOG_ERR("Failed to configure backlight GPIO: %d", ret);
        return;
    }

    ret = gpio_pin_set_dt(&bl_led, state ? 1 : 0);
    if (ret < 0)
    {
        LOG_ERR("Failed to set backlight GPIO: %d", ret);
        return;
    }

    LOG_DBG("Backlight set to %s", state ? "ON" : "OFF");
}

int display_init(void)
{
    if (!DT_NODE_EXISTS(DT_NODELABEL(ili9341)))
    {
        LOG_ERR("ILI9341 node not found in devicetree");
        return -ENODEV;
    }

    display_dev = DEVICE_DT_GET(DT_NODELABEL(ili9341));
    if (!display_dev)
    {
        LOG_ERR("Failed to get display device");
        return -ENODEV;
    }

    if (!device_is_ready(display_dev))
    {
        LOG_ERR("Display device not ready");
        return -EIO;
    }

    display_backlight_set(true);
    LOG_INF("Display initialized successfully");
    return 0;
}

void display_test_pattern(void)
{
    if (!device_is_ready(display_dev))
    {
        LOG_ERR("Display device not ready");
        return;
    }

    struct display_capabilities capabilities;
    display_get_capabilities(display_dev, &capabilities);

    const uint16_t width = capabilities.x_resolution;
    const uint16_t height = capabilities.y_resolution;
    LOG_DBG("Display resolution: %dx%d", width, height);

    struct display_buffer_descriptor desc = {
        .width = width,
        .height = height,
        .pitch = width,
        .buf_size = width * height * sizeof(uint16_t)};

    uint16_t *buf = k_malloc(desc.buf_size);
    if (!buf)
    {
        LOG_ERR("Failed to allocate display buffer");
        return;
    }

    /* Fill with solid red (RGB565: 0xF800) */
    for (uint32_t i = 0; i < width * height; i++)
    {
        buf[i] = 0xF800; /* Red in RGB565 */
    }

    int ret = display_write(display_dev, 0, 0, &desc, buf);
    if (ret < 0)
    {
        LOG_ERR("Failed to write to display: %d", ret);
    }
    else
    {
        LOG_INF("Test pattern (solid red) written to display");
    }

    k_free(buf);
}