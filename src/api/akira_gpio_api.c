/**
 * @file akira_gpio_api.c
 * @brief GPIO API implementation for WASM exports
 */

#include "akira_api.h"
#include "akira_gpio_api.h"
#include <runtime/security.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <errno.h>

LOG_MODULE_REGISTER(akira_gpio_api, CONFIG_AKIRA_LOG_LEVEL);

/* GPIO device handles */
static const struct device *gpio0_dev = NULL;
static const struct device *gpio1_dev = NULL;

/* Initialize GPIO devices on first use */
static int akira_gpio_init(void)
{
    static bool initialized = false;
    
    if (initialized) {
        return 0;
    }
    
#if defined(CONFIG_GPIO)
#if DT_NODE_HAS_STATUS(DT_NODELABEL(gpio0), okay)
    gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio0_dev)) {
        LOG_ERR("GPIO0 device not ready");
        gpio0_dev = NULL;
    }
#else
    gpio0_dev = NULL;
    LOG_DBG("GPIO0 not defined in device tree");
#endif
    
#if DT_NODE_HAS_STATUS(DT_NODELABEL(gpio1), okay)
    gpio1_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));
    if (!device_is_ready(gpio1_dev)) {
        LOG_DBG("GPIO1 device not available (normal for some platforms)");
        gpio1_dev = NULL;
    }
#else
    gpio1_dev = NULL;
    LOG_DBG("GPIO1 not defined in device tree");
#endif
    
    if (!gpio0_dev && !gpio1_dev) {
        LOG_ERR("No GPIO devices available");
        return -ENODEV;
    }
#else
    gpio0_dev = NULL;
    gpio1_dev = NULL;
    LOG_WRN("GPIO subsystem not enabled (CONFIG_GPIO=n)");
    return -ENOTSUP;
#endif
    
    initialized = true;
    LOG_INF("GPIO API initialized");
    return 0;
}

/* Get GPIO device based on pin number */
static const struct device *akira_gpio_get_device(uint32_t pin, gpio_pin_t *pin_out)
{
    if (akira_gpio_init() != 0) {
        return NULL;
    }
    
    /* ESP32-S3: GPIO0-31 on gpio0, GPIO32-48 on gpio1 */
    if (pin < 32) {
        *pin_out = pin;
        return gpio0_dev;
    } else if (pin < 49 && gpio1_dev) {
        *pin_out = pin - 32;
        return gpio1_dev;
    }
    
    LOG_ERR("Invalid GPIO pin: %u", pin);
    return NULL;
}

/* Convert AKIRA GPIO flags to Zephyr GPIO flags */
static gpio_flags_t akira_gpio_flags_to_zephyr(uint32_t akira_flags)
{
    gpio_flags_t zephyr_flags = 0;
    
    if (akira_flags & AKIRA_GPIO_INPUT) {
        zephyr_flags |= GPIO_INPUT;
    }
    if (akira_flags & AKIRA_GPIO_OUTPUT) {
        zephyr_flags |= GPIO_OUTPUT;
    }
    if (akira_flags & AKIRA_GPIO_OUTPUT_INIT_LOW) {
        zephyr_flags |= GPIO_OUTPUT_INIT_LOW;
    }
    if (akira_flags & AKIRA_GPIO_OUTPUT_INIT_HIGH) {
        zephyr_flags |= GPIO_OUTPUT_INIT_HIGH;
    }
    if (akira_flags & AKIRA_GPIO_PULL_UP) {
        zephyr_flags |= GPIO_PULL_UP;
    }
    if (akira_flags & AKIRA_GPIO_PULL_DOWN) {
        zephyr_flags |= GPIO_PULL_DOWN;
    }
    if (akira_flags & AKIRA_GPIO_ACTIVE_LOW) {
        zephyr_flags |= GPIO_ACTIVE_LOW;
    }
    if (akira_flags & AKIRA_GPIO_ACTIVE_HIGH) {
        zephyr_flags |= GPIO_ACTIVE_HIGH;
    }
    
    return zephyr_flags;
}

int akira_gpio_configure(uint32_t pin, uint32_t flags)
{
    gpio_pin_t gpio_pin = 0;
    const struct device *dev = akira_gpio_get_device(pin, &gpio_pin);
    
    if (!dev) {
        return -EINVAL;
    }
    
    gpio_flags_t zephyr_flags = akira_gpio_flags_to_zephyr(flags);
    int ret = gpio_pin_configure(dev, gpio_pin, zephyr_flags);
    
    if (ret < 0) {
        LOG_ERR("Failed to configure GPIO %u: %d", pin, ret);
        return ret;
    }
    
    LOG_DBG("Configured GPIO %u with flags 0x%08x", pin, flags);
    return 0;
}

int akira_gpio_read(uint32_t pin)
{
    gpio_pin_t gpio_pin = 0;
    const struct device *dev = akira_gpio_get_device(pin, &gpio_pin);
    
    if (!dev) {
        return -EINVAL;
    }
    
    int ret = gpio_pin_get(dev, gpio_pin);
    
    if (ret < 0) {
        LOG_ERR("Failed to read GPIO %u: %d", pin, ret);
        return ret;
    }
    
    return ret;
}

int akira_gpio_write(uint32_t pin, uint32_t value)
{
    gpio_pin_t gpio_pin = 0;
    const struct device *dev = akira_gpio_get_device(pin, &gpio_pin);
    
    if (!dev) {
        return -EINVAL;
    }
    
    int ret = gpio_pin_set(dev, gpio_pin, value ? 1 : 0);
    
    if (ret < 0) {
        LOG_ERR("Failed to write GPIO %u: %d", pin, ret);
        return ret;
    }
    
    return 0;
}

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM Native export API */

int akira_native_gpio_configure(wasm_exec_env_t exec_env, uint32_t pin, uint32_t flags)
{
    /* Output pin requires write capability; input pin requires read capability */
    uint64_t required_cap = (flags & AKIRA_GPIO_OUTPUT) ?
                            AKIRA_CAP_GPIO_WRITE : AKIRA_CAP_GPIO_READ;

    AKIRA_CHECK_CAP_OR_RETURN(exec_env, required_cap, -EPERM);
    
    return akira_gpio_configure(pin, flags);
}

int akira_native_gpio_read(wasm_exec_env_t exec_env, uint32_t pin)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_GPIO_READ, -EPERM);
    return akira_gpio_read(pin);
}

int akira_native_gpio_write(wasm_exec_env_t exec_env, uint32_t pin, uint32_t value)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_GPIO_WRITE, -EPERM);
    return akira_gpio_write(pin, value);
}

#endif

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_API) && defined(CONFIG_GPIO))
#include <akira_native_registry.h>

static const NativeSymbol akira_gpio_natives[] = {
    {"gpio_configure", (void *)akira_native_gpio_configure, "(ii)i", NULL},
    {"gpio_read", (void *)akira_native_gpio_read, "(i)i", NULL},
    {"gpio_write", (void *)akira_native_gpio_write, "(ii)i", NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_gpio_api, "env", akira_gpio_natives);
#endif
