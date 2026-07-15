/**
 * @file akira_mqtt_api.c
 * @brief MQTT / Home Assistant WASM native API — bridges apps to mqtt_service.
 */

#include "akira_mqtt_api.h"
#include <connectivity/mqtt/mqtt_service.h>
#include <connectivity/mqtt/ha_discovery.h>
#include <runtime/security.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <wasm_export.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(akira_mqtt_api, CONFIG_AKIRA_LOG_LEVEL);

/* ---- Generic MQTT ------------------------------------------------------- */

/* mqtt_publish — sig "($*~ii)i" */
int akira_native_mqtt_publish(wasm_exec_env_t exec_env, const char *topic,
                              const void *payload, int len, int qos, int retain)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    if (!topic || len < 0) {
        return -EINVAL;
    }
    return mqtt_service_publish(topic, payload, (size_t)len, qos, retain != 0);
}

/* mqtt_subscribe — sig "($)i" */
int akira_native_mqtt_subscribe(wasm_exec_env_t exec_env, const char *topic)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    if (!topic) {
        return -EINVAL;
    }
    return mqtt_service_subscribe(topic);
}

/* mqtt_poll — sig "(*~*~i)i" : topic, topic_cap, payload, payload_cap, timeout */
int akira_native_mqtt_poll(wasm_exec_env_t exec_env, char *topic, int topic_cap,
                           uint8_t *payload, int payload_cap, int timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    if (!topic || !payload || topic_cap <= 0 || payload_cap <= 0) {
        return -EINVAL;
    }

    struct akira_mqtt_message msg;
    k_timeout_t to = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);
    int rc = mqtt_service_poll_message(&msg, to);
    if (rc != 0) {
        return -EAGAIN;
    }

    int tn = MIN((int)strlen(msg.topic), topic_cap - 1);
    memcpy(topic, msg.topic, tn);
    topic[tn] = '\0';

    int pn = MIN(payload_cap, (int)msg.payload_len);
    if (pn > 0) {
        memcpy(payload, msg.payload, pn);
    }
    return pn;
}

/* mqtt_connected — sig "()i" */
int akira_native_mqtt_connected(wasm_exec_env_t exec_env)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    return mqtt_service_is_connected() ? 1 : 0;
}

/* ---- Home Assistant light helpers --------------------------------------- */

/* ha_light_register — sig "($$)i" */
int akira_native_ha_light_register(wasm_exec_env_t exec_env,
                                   const char *object_id, const char *name)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    if (!object_id || !name) {
        return -EINVAL;
    }
    return ha_light_register(object_id, name);
}

/* ha_light_report — sig "($iiiii)i" */
int akira_native_ha_light_report(wasm_exec_env_t exec_env, const char *object_id,
                                 int on, int brightness, int r, int g, int b)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    if (!object_id) {
        return -EINVAL;
    }
    return ha_light_report(object_id, on, brightness, r, g, b);
}

/* ha_light_poll — sig "($*****i)i" */
int akira_native_ha_light_poll(wasm_exec_env_t exec_env, const char *object_id,
                               int *on, int *brightness, int *r, int *g, int *b,
                               int timeout_ms)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, AKIRA_CAP_MQTT, -EPERM);
    if (!object_id || !on || !brightness || !r || !g || !b) {
        return -EINVAL;
    }
    return ha_light_poll(object_id, on, brightness, r, g, b, timeout_ms);
}
