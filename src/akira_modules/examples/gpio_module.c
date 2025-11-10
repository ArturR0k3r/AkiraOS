/*
 * SPDX-License-Identifier: Apache-2.0
 * 
 * Akira GPIO Module - GPIO pin control
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <akira_module.h>

LOG_MODULE_REGISTER(akira_gpio_module, CONFIG_LOG_DEFAULT_LEVEL);

struct gpio_cmd {
	uint32_t pin;
	uint32_t value;
};

static const struct device *gpio_dev;

static int gpio_module_init(void *user_data)
{
	gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	if (!device_is_ready(gpio_dev)) {
		LOG_ERR("GPIO device not ready");
		return -ENODEV;
	}
	
	LOG_INF("GPIO module initialized");
	return 0;
}

static int gpio_module_command(const char *command, void *data,
                                size_t len, void *user_data)
{
	if (!data || len < sizeof(struct gpio_cmd)) {
		return -EINVAL;
	}
	
	struct gpio_cmd *cmd = (struct gpio_cmd *)data;
	
	if (strcmp(command, "configure_output") == 0) {
		int ret = gpio_pin_configure(gpio_dev, cmd->pin, GPIO_OUTPUT);
		LOG_DBG("Configured pin %d as output: %d", cmd->pin, ret);
		return ret;
	}
	else if (strcmp(command, "configure_input") == 0) {
		int ret = gpio_pin_configure(gpio_dev, cmd->pin, GPIO_INPUT);
		LOG_DBG("Configured pin %d as input: %d", cmd->pin, ret);
		return ret;
	}
	else if (strcmp(command, "set") == 0) {
		int ret = gpio_pin_set(gpio_dev, cmd->pin, cmd->value);
		LOG_DBG("Set pin %d to %d: %d", cmd->pin, cmd->value, ret);
		return ret;
	}
	else if (strcmp(command, "get") == 0) {
		cmd->value = gpio_pin_get(gpio_dev, cmd->pin);
		LOG_DBG("Read pin %d: %d", cmd->pin, cmd->value);
		return 0;
	}
	else if (strcmp(command, "toggle") == 0) {
		int ret = gpio_pin_toggle(gpio_dev, cmd->pin);
		LOG_DBG("Toggled pin %d: %d", cmd->pin, ret);
		return ret;
	}
	
	LOG_WRN("Unknown GPIO command: %s", command);
	return -ENOTSUP;
}

AKIRA_MODULE_DEFINE(gpio,
                    AKIRA_MODULE_TYPE_GPIO,
                    gpio_module_init,
                    NULL,
                    gpio_module_command,
                    NULL,
                    NULL);
