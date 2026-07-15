/**
 * @file ha_discovery.h
 * @brief Home Assistant MQTT-Discovery helpers (light entity).
 *
 * Builds/publishes HA MQTT-Discovery config + state and parses inbound
 * command payloads, on top of mqtt_service. The JSON build/parse routines are
 * pure functions (no broker needed) so they can be unit-tested directly.
 *
 * Topic layout (node = mqtt_service_node_id()):
 *   config : homeassistant/light/<node>_<object>/config   (retained)
 *   state  : <node>/light/<object>/state                  (retained)
 *   command: <node>/light/<object>/set
 *
 * @stability experimental
 * @since 1.6
 */

#ifndef AKIRA_HA_DISCOVERY_H
#define AKIRA_HA_DISCOVERY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fields present in a parsed HA light command (bitmask). */
#define HA_CMD_STATE       (1 << 0)
#define HA_CMD_BRIGHTNESS  (1 << 1)
#define HA_CMD_COLOR       (1 << 2)

/* ---- High-level API (used by the WASM ha_light_* natives) ---------------- */

/**
 * Announce a Home Assistant light entity via MQTT discovery and subscribe to
 * its command topic. Idempotent per object_id.
 * @return 0 on success, negative errno on failure (e.g. -ENOTCONN).
 */
int ha_light_register(const char *object_id, const char *name);

/**
 * Publish the current light state (retained) to the entity's state topic.
 * @param on         non-zero = ON.
 * @param brightness 0..255.
 * @param r,g,b      0..255 color.
 * @return 0 on success, negative errno on failure.
 */
int ha_light_report(const char *object_id, int on, int brightness,
                    int r, int g, int b);

/**
 * Wait for the next command targeting @p object_id and decode it.
 * @return bitmask of HA_CMD_* fields present (>0), 0 on timeout,
 *         negative errno on error.
 */
int ha_light_poll(const char *object_id, int *on, int *brightness,
                  int *r, int *g, int *b, int timeout_ms);

/* ---- Pure helpers (also unit-tested) ------------------------------------- */

/** Build the retained discovery config JSON. @return length, or negative. */
int ha_build_light_config(char *buf, size_t cap, const char *node,
                          const char *object, const char *name);

/** Build the state JSON. @return length, or negative. */
int ha_build_light_state(char *buf, size_t cap, int on, int brightness,
                         int r, int g, int b);

/**
 * Parse an HA JSON light command payload.
 * Fills only the fields present; unfilled out-params are left untouched.
 * @return bitmask of HA_CMD_* fields found (0 if none).
 */
int ha_parse_light_command(const char *json, size_t len, int *on,
                           int *brightness, int *r, int *g, int *b);

#ifdef __cplusplus
}
#endif

#endif /* AKIRA_HA_DISCOVERY_H */
