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

#ifdef CONFIG_AKIRA_WASM_DISPLAY
#include "akira_display_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_INPUT
#include "akira_input_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_RF
#include "akira_rf_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_SENSOR
#include "akira_sensor_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_MEMORY
#include "akira_memory_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_BT_SHELL
#include "akira_bt_shell_api.h"
#endif

#endif /* AKIRA_API_H */
