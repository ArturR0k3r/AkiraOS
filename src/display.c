#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

static const struct device *display_dev;

void display_backlight_set(bool state)
{
    const struct gpio_dt_spec bl_led = GPIO_DT_SPEC_GET(DT_NODELABEL(ili9341), led_gpios);

    if (!device_is_ready(bl_led.port))
    {
        LOG_ERR("Backlight LED device not ready");
        return;
    }

    gpio_pin_set_dt(&bl_led, (int)state);
}

int display_init(void)
{
    display_dev = DEVICE_DT_GET(DT_NODELABEL(ili9341));

    if (!device_is_ready(display_dev))
    {
        LOG_ERR("Display device not ready");
        return -ENODEV;
    }

    display_backlight_set(true);
    LOG_INF("Display initialized");

    return 0;
}

void display_test_pattern(void)
{
    struct display_capabilities capabilities;
    display_get_capabilities(display_dev, &capabilities);

    const uint16_t width = capabilities.x_resolution;
    const uint16_t height = capabilities.y_resolution;

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

    // Draw gradient
    for (uint16_t y = 0; y < height; y++)
    {
        for (uint16_t x = 0; x < width; x++)
        {
            buf[y * width + x] = ((x * 255 / width) << 8) | (y * 255 / height);
        }
    }

    display_write(display_dev, 0, 0, &desc, buf);
    k_free(buf);
}