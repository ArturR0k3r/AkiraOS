/**
 * @file akira_api.h
 * @brief AkiraOS WASM API Exports
 *
 * All functions exported to WASM applications.
 * Each API requires specific capabilities granted in app manifest.
 */

#ifndef AKIRA_API_H
#define AKIRA_API_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef CONFIG_AKIRA_WASM_RUNTIME
    #include <wasm_export.h>
    bool akira_register_native_apis(void);
    #include "akira_common_api.h"
#endif

#ifdef CONFIG_DISPLAY
#include "akira_display_api.h"
#endif


#if defined(CONFIG_AKIRA_WASM_API) && defined(CONFIG_GPIO)
#include "akira_gpio_api.h"
#endif

#if defined(CONFIG_AKIRA_WASM_API) && defined(CONFIG_AKIRA_MODULE_RF) && defined(CONFIG_AKIRA_RF_FRAMEWORK)
#include "akira_rf_api.h"
#endif

#if defined(CONFIG_AKIRA_WASM_API) && defined(CONFIG_SENSOR)
#include "akira_sensor_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_MEMORY
#include "akira_memory_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_BLE
#include "akira_ble_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_TIMER
#include "akira_timer_api.h"
#endif

#ifdef CONFIG_AKIRA_SYSTEM_API
#include "akira_system_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_UART
#include "akira_uart_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_I2C
#include "akira_i2c_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_PWM
#include "akira_pwm_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_HID
#include "akira_hid_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_LIFECYCLE
#include "akira_lifecycle_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_IPC
#include "akira_ipc_api.h"
#endif

#endif /* AKIRA_API_H */
