/**
 * @file akira_gpio_api.h
 * @brief GPIO API declarations for WASM exports
 * @stability stable
 * @since 1.3
 */

#ifndef AKIRA_GPIO_API_H
#define AKIRA_GPIO_API_H

#include <wasm_export.h>
#include <stdint.h>
#include <stdbool.h>

/* GPIO flags - mirror Zephyr's GPIO flags for WASM apps */
#define AKIRA_GPIO_INPUT            (1U << 0)
#define AKIRA_GPIO_OUTPUT           (1U << 1)
#define AKIRA_GPIO_OUTPUT_INIT_LOW  (1U << 2)
#define AKIRA_GPIO_OUTPUT_INIT_HIGH (1U << 3)
#define AKIRA_GPIO_PULL_UP          (1U << 4)
#define AKIRA_GPIO_PULL_DOWN        (1U << 5)
#define AKIRA_GPIO_ACTIVE_LOW       (1U << 6)
#define AKIRA_GPIO_ACTIVE_HIGH      (1U << 7)

/* Core GPIO API functions (no capability checks). Pin numbering is the SoC
 * GPIO number: 0-31 on gpio0, 32+ on gpio1. SoC-reserved pins (ESP32-S3:
 * flash/PSRAM GPIO26-37, USB GPIO19/20, UART0 GPIO43/44) are refused with
 * -EACCES for every caller: reconfiguring them stalls or kills the system.
 * Contradictory flags (both pulls, both initial levels, an initial level
 * without output) are refused with -EINVAL. */
int akira_gpio_configure(uint32_t pin, uint32_t flags);
int akira_gpio_read(uint32_t pin);
int akira_gpio_write(uint32_t pin, uint32_t value);

/**
 * True if WASM apps may use @p pin: in CONFIG_AKIRA_WASM_GPIO_APP_PINS, not
 * SoC-reserved or strapping, and not given to a system device by the
 * devicetree (any okay node's *-gpios or pinctrl-0 pads).
 */
bool akira_gpio_pin_app_allowed(uint32_t pin);

/**
 * True if @p pin belongs to the system (SoC-reserved, strapping, or used by
 * an okay devicetree node's GPIO or pinctrl properties), whatever the app
 * allowlist says. Lets APIs that would reconfigure a pad (ADC) refuse it.
 */
bool akira_gpio_pin_is_system(uint32_t pin);

/**
 * True if @p pin may carry the output of a PWM channel an app drives: as
 * akira_gpio_pin_app_allowed(), except that being an output pad of the
 * PWM (LEDC) controller's own pinctrl does not count as a system use.
 */
bool akira_gpio_pin_pwm_pad_allowed(uint32_t pin);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM native export functions (with capability checks). An app may only
 * read or write a pin it configured itself; every pin it configured is
 * disconnected again when the app ends, however it ends. */
int akira_native_gpio_configure(wasm_exec_env_t exec_env, uint32_t pin, uint32_t flags);
int akira_native_gpio_read(wasm_exec_env_t exec_env, uint32_t pin);
int akira_native_gpio_write(wasm_exec_env_t exec_env, uint32_t pin, uint32_t value);
#endif

#endif /* AKIRA_GPIO_API_H */
