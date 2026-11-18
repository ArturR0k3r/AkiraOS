#include "akira_api.h"
#include "akira_storage_api.h"
#include "akira_net_api.h"
#include "akira_power_api.h"
#ifdef CONFIG_AKIRA_WASM_INPUT
#include "akira_input_api.h"
#endif
#ifdef CONFIG_AKIRA_WASM_SETTINGS
#include "akira_settings_api.h"
#endif
#ifdef CONFIG_AKIRA_SYSTEM_API
#include "akira_system_api.h"
#endif
#ifdef CONFIG_AKIRA_WASM_ADC
#include "akira_adc_api.h"
#endif
#ifdef CONFIG_AKIRA_WASM_NFC
#include "akira_nfc_api.h"
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
#ifdef CONFIG_AKIRA_CRYSTAL
#include <crystal/akira_seal.h>
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

/* The natives below are defined in console source files that carry no table of
 * their own (akira_input_api.c, akira_nfc_api.c, crystal/akira_seal_api.c,
 * akira_system_api.c's run-from-SD path); the console branch registered them in
 * this file's old central table. Keep them here as per-file tables so they are
 * still exported under the v1.7.x registry. Do NOT also add them to those
 * files' own tables, or the runtime refuses to start (duplicate (module,name)). */

/* input.read: button bitmask, edge events, and rotary dial */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && defined(CONFIG_AKIRA_WASM_INPUT)
static const NativeSymbol akira_input_natives[] = {
    {"input_get_buttons", (void *)akira_native_input_get_buttons, "()i",   NULL},
    {"input_poll_event",  (void *)akira_native_input_poll_event,  "(*~)i", NULL},
    {"input_get_dial",    (void *)akira_native_input_get_dial,    "()i",   NULL},
};
AKIRA_NATIVE_API_DEFINE(akira_input_api, "env", akira_input_natives);
#endif

/* nfc.*: tag UID, EEPROM read/write, RF field detect, fast-transfer mailbox */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && defined(CONFIG_AKIRA_WASM_NFC)
static const NativeSymbol akira_nfc_natives[] = {
    {"nfc_uid",           (void *)akira_native_nfc_uid,           "(*)i",   NULL},
    {"nfc_read",          (void *)akira_native_nfc_read,          "(i*~)i", NULL},
    {"nfc_write",         (void *)akira_native_nfc_write,         "(i*~)i", NULL},
    {"nfc_field_present", (void *)akira_native_nfc_field_present,  "()i",    NULL},
    {"nfc_mb_enable",     (void *)akira_native_nfc_mb_enable,      "(ii)i",  NULL},
    {"nfc_mb_put",        (void *)akira_native_nfc_mb_put,         "(*~)i",  NULL},
    {"nfc_mb_get",        (void *)akira_native_nfc_mb_get,         "(*~)i",  NULL},
    {"nfc_mb_status",     (void *)akira_native_nfc_mb_status,      "(**)i",  NULL},
};
AKIRA_NATIVE_API_DEFINE(akira_nfc_api, "env", akira_nfc_natives);
#endif

/* Akira & the Seal: ask for a power, award a glint, seal a record */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && defined(CONFIG_AKIRA_CRYSTAL)
static const NativeSymbol akira_seal_natives[] = {
    {"seal_request", (void *)akira_native_seal_request, "(i$)i",  NULL},
    {"akira_glint",  (void *)akira_native_akira_glint,  "(i)i",   NULL},
    {"seal_record",  (void *)akira_native_seal_record,  "($I)i",  NULL},
};
AKIRA_NATIVE_API_DEFINE(akira_seal_api, "env", akira_seal_natives);
#endif

/* system: run a WASM app straight from the SD card (XIP) */
#if defined(CONFIG_AKIRA_WASM_RUNTIME) && defined(CONFIG_AKIRA_WASM_API) && defined(CONFIG_AKIRA_SYSTEM_API) && defined(CONFIG_AKIRA_SD_XIP)
static const NativeSymbol akira_sd_xip_natives[] = {
    {"app_run_from_sd", (void *)akira_native_app_run_from_sd, "($)i", NULL},
};
AKIRA_NATIVE_API_DEFINE(akira_sd_xip_api, "env", akira_sd_xip_natives);
#endif

/* Deprecated since 1.6: the runtime registers every AKIRA_NATIVE_API_DEFINE()
 * table itself. Kept so existing callers keep linking. */
bool akira_register_native_apis(void)
{
    return akira_native_registry_register_all() == 0;
}
