/**
 * @file akira_gpio_api.h
 * @brief GPIO API declarations for WASM exports
 * @stability stable
 * @since 1.3
 */

#ifndef AKIRA_GPIO_API_H
#define AKIRA_GPIO_API_H

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#endif
#include <stdint.h>

/* GPIO flags - mirror Zephyr's GPIO flags for WASM apps */
#define AKIRA_GPIO_INPUT            (1U << 0)
#define AKIRA_GPIO_OUTPUT           (1U << 1)
#define AKIRA_GPIO_OUTPUT_INIT_LOW  (1U << 2)
#define AKIRA_GPIO_OUTPUT_INIT_HIGH (1U << 3)
#define AKIRA_GPIO_PULL_UP          (1U << 4)
#define AKIRA_GPIO_PULL_DOWN        (1U << 5)
#define AKIRA_GPIO_ACTIVE_LOW       (1U << 6)
#define AKIRA_GPIO_ACTIVE_HIGH      (1U << 7)

/* Core GPIO API functions (no security checks) */
int akira_gpio_configure(uint32_t pin, uint32_t flags);
int akira_gpio_read(uint32_t pin);
int akira_gpio_write(uint32_t pin, uint32_t value);

#ifdef CONFIG_AKIRA_WASM_RUNTIME
/* WASM native export functions (with capability checks) */
int akira_native_gpio_configure(wasm_exec_env_t exec_env, uint32_t pin, uint32_t flags);
int akira_native_gpio_read(wasm_exec_env_t exec_env, uint32_t pin);
int akira_native_gpio_write(wasm_exec_env_t exec_env, uint32_t pin, uint32_t value);
#endif

#endif /* AKIRA_GPIO_API_H */
