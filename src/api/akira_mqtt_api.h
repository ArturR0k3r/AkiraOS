/**
 * @file akira_mqtt_api.h
 * @brief MQTT / Home Assistant WASM native API.
 *
 * Exposes generic MQTT publish/subscribe/poll plus Home Assistant light
 * helpers to WASM apps, on top of mqtt_service + ha_discovery. All functions
 * require the "mqtt" manifest capability (AKIRA_CAP_MQTT).
 *
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_MQTT_API_H
#define AKIRA_MQTT_API_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_AKIRA_WASM_RUNTIME
#include <wasm_export.h>
#else
typedef void *wasm_exec_env_t;
#endif

#include <stdint.h>

/* ---- Generic MQTT ---- */
int akira_native_mqtt_publish(wasm_exec_env_t exec_env, const char *topic,
                              const void *payload, int len, int qos, int retain);
int akira_native_mqtt_subscribe(wasm_exec_env_t exec_env, const char *topic);
int akira_native_mqtt_poll(wasm_exec_env_t exec_env, char *topic, int topic_cap,
                           uint8_t *payload, int payload_cap, int timeout_ms);
int akira_native_mqtt_connected(wasm_exec_env_t exec_env);

/* ---- Home Assistant light helpers ---- */
int akira_native_ha_light_register(wasm_exec_env_t exec_env,
                                   const char *object_id, const char *name);
int akira_native_ha_light_report(wasm_exec_env_t exec_env, const char *object_id,
                                 int on, int brightness, int r, int g, int b);
int akira_native_ha_light_poll(wasm_exec_env_t exec_env, const char *object_id,
                               int *on, int *brightness, int *r, int *g, int *b,
                               int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MQTT_API_H */
