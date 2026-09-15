#include "akira_api.h"
#include "akira_storage_api.h"
#include "akira_net_api.h"
#include "akira_power_api.h"
#ifdef CONFIG_AKIRA_WASM_SETTINGS
#include "akira_settings_api.h"
#endif
#ifdef CONFIG_AKIRA_SYSTEM_API
#include "akira_system_api.h"
#endif
#ifdef CONFIG_AKIRA_WASM_ADC
#include "akira_adc_api.h"
#endif
#ifdef CONFIG_AKIRA_WASM_WDT
#include "akira_wdt_api.h"
#endif
#ifdef CONFIG_AKIRA_WASM_FS
#include "akira_fs_api.h"
#endif
#ifdef CONFIG_AKIRA_WASM_CRYPTO
#include "akira_crypto_api.h"
#endif
#ifdef CONFIG_AKIRA_WASM_RTC
#include "akira_rtc_api.h"
#endif
#ifdef CONFIG_AKIRA_WASM_OTA
#include "akira_ota_api.h"
#endif
#ifdef CONFIG_AKIRA_WASM_AIINFER
#include "akira_aiinfer_api.h"
#endif
#ifdef CONFIG_AKIRA_WASM_MATTER
#include "akira_matter_api.h"
#endif
#ifdef CONFIG_AKIRA_WASM_MQTT
#include "akira_mqtt_api.h"
#endif

#ifdef CONFIG_AKIRA_WASM_MESH
#include "akira_mesh_api.h"
#endif

#include <runtime/akira_runtime.h>
#include <runtime/security.h>
#include <zephyr/logging/log.h>
#include <stdbool.h>
#include <string.h>

#include <wasm_export.h>
#include <akira_native_registry.h>
#ifdef CONFIG_AKIRA_WASM_PYTHON
#include "akira_python_api.h"
#endif

LOG_MODULE_REGISTER(akira_export_api, CONFIG_AKIRA_LOG_LEVEL);

/* ===== WASM exports =====
 * Registered with WAMR by the native API registry (akira_native_registry.h).
 * Import names and signatures are WASM ABI: see docs/api-stability-policy.md. */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && (defined(CONFIG_AKIRA_WASM_AIINFER))
#include <akira_native_registry.h>

/* ai.infer: on-device ML inference via TFLite Micro (AkiraClaw) */
static const NativeSymbol akira_aiinfer_natives[] = {
    {"aiinfer_load",   (void *)akira_native_aiinfer_load,   "(*~)i",    NULL},
    {"aiinfer_run",    (void *)akira_native_aiinfer_run,    "(i*~*~)i", NULL},
    {"aiinfer_unload", (void *)akira_native_aiinfer_unload, "(i)",      NULL},
};

AKIRA_NATIVE_API_DEFINE(akira_aiinfer_api, "env", akira_aiinfer_natives);
#endif

/* Deprecated since 1.6: the runtime registers every AKIRA_NATIVE_API_DEFINE()
 * table itself. Kept so existing callers keep linking. */
bool akira_register_native_apis(void)
{
    return akira_native_registry_register_all() == 0;
}
