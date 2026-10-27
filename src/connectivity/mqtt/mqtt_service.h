/**
 * @file mqtt_service.h
 * @brief AkiraOS MQTT client service (Home Assistant integration).
 *
 * Owns a single MQTT connection to a broker (e.g. Mosquitto / the Home
 * Assistant Mosquitto add-on) over the device WiFi link. Connects on WiFi-up
 * (when CONFIG_AKIRA_MQTT_AUTO_CONNECT), reconnects on drops, and exposes
 * publish/subscribe plus an inbound-message queue for consumers
 * (the WASM mqtt_* API and the Home Assistant light helpers).
 *
 * Broker address/credentials come from NVS settings (mqtt/host, mqtt/port,
 * mqtt/user, mqtt/pass), falling back to the CONFIG_AKIRA_MQTT_BROKER_*
 * defaults.
 *
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_MQTT_SERVICE_H
#define AKIRA_MQTT_SERVICE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AKIRA_MQTT_TOPIC_MAX    80
#define AKIRA_MQTT_PAYLOAD_MAX   160

typedef enum {
    MQTT_SVC_DISCONNECTED = 0,
    MQTT_SVC_CONNECTING,
    MQTT_SVC_CONNECTED,
} mqtt_svc_state_t;

/* One inbound MQTT PUBLISH, as delivered to consumers. */
struct akira_mqtt_message {
    char     topic[AKIRA_MQTT_TOPIC_MAX];
    uint8_t  payload[AKIRA_MQTT_PAYLOAD_MAX];
    uint16_t payload_len;
};

/**
 * Initialise the MQTT service (idempotent). Registers the WiFi-up callback
 * when CONFIG_AKIRA_MQTT_AUTO_CONNECT. Called automatically at boot via
 * SYS_INIT; safe to call again.
 */
int mqtt_service_init(void);

/** Request a connection to the broker (spawns/kicks the service thread). */
int mqtt_service_start(void);

/** Disconnect and stop reconnecting. */
int mqtt_service_stop(void);

/** @return current connection state. */
mqtt_svc_state_t mqtt_service_get_state(void);

/** @return true if fully connected (CONNACK received). */
bool mqtt_service_is_connected(void);

/**
 * Publish a message.
 * @param topic   NUL-terminated topic string.
 * @param payload Payload bytes (may be NULL when len == 0).
 * @param len     Payload length.
 * @param qos     0 or 1 (2 is treated as 1).
 * @param retain  Set the retain flag.
 * @return 0 on success, negative errno on failure (e.g. -ENOTCONN).
 */
int mqtt_service_publish(const char *topic, const void *payload, size_t len,
                         int qos, bool retain);

/** Subscribe to a topic filter (QoS 0). */
int mqtt_service_subscribe(const char *topic);

/**
 * Pop the next inbound message (blocking up to @p timeout).
 * @return 0 on success, -EAGAIN on timeout, negative errno otherwise.
 */
int mqtt_service_poll_message(struct akira_mqtt_message *out, k_timeout_t timeout);

/**
 * Update broker address/credentials (persisted to NVS). An empty user/pass
 * clears that field. Does not reconnect — call mqtt_service_start().
 */
int mqtt_service_set_broker(const char *host, uint16_t port,
                            const char *user, const char *pass);

/** @return the Home Assistant node id (e.g. "akira_a1b2c3"). */
const char *mqtt_service_node_id(void);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_MQTT_SERVICE_H */
