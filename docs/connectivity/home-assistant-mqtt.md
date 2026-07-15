# Home Assistant over MQTT

AkiraOS can expose its hardware to **Home Assistant** over MQTT, using HA's
**MQTT Discovery** so entities appear automatically. Unlike the Matter path
(which needs a co-processor), MQTT runs entirely on the device's WiFi — no
extra hardware.

- Firmware owns the broker connection (`src/connectivity/mqtt/mqtt_service.c`).
- HA discovery + light helpers live in `src/connectivity/mqtt/ha_discovery.c`.
- WASM apps declare entities through the `mqtt_*` / `ha_light_*` host API
  (capability `"mqtt"`). Reference app: `AkiraSDK/wasm_apps/generic/ha_rgb/`.

## Enabling

```kconfig
CONFIG_AKIRA_MQTT=y            # MQTT client service
CONFIG_AKIRA_WASM_MQTT=y       # mqtt_* / ha_light_* WASM natives
# CONFIG_AKIRA_MQTT_TLS=y      # optional: TLS on port 8883 (needs a CA cert)
```

`CONFIG_AKIRA_MQTT_AUTO_CONNECT=y` (default) connects to the broker as soon as
WiFi comes up, via a `wifi_manager` callback, and reconnects on drops.

On the DRAM-tight `esp32s3_super_mini`, build with the ready-made fragment:

```sh
west build -p -b esp32s3_super_mini/esp32s3/procpu . \
  -- -DEXTRA_CONF_FILE=AkiraSDK/wasm_apps/generic/ha_rgb/esp32s3_demo.conf
```

## Broker configuration

Address/credentials are stored in NVS (`mqtt/host`, `mqtt/port`, `mqtt/user`,
`mqtt/pass`; the password is stored encrypted), falling back to the
`CONFIG_AKIRA_MQTT_BROKER_*` defaults. Configure at runtime from the shell:

```
mqtt set <host> <port> [user] [pass]    # e.g. mqtt set homeassistant.local 1883 akira secret
mqtt connect
mqtt status                             # connection state + node id
mqtt pub <topic> <payload> [retain]     # manual publish
```

## Home Assistant setup

1. Install a broker — the **Mosquitto broker** add-on in HA is easiest.
2. Add the **MQTT integration** (Settings → Devices & Services → Add → MQTT)
   pointed at that broker. MQTT Discovery is on by default (prefix
   `homeassistant`).
3. Run an entity app (below). It publishes a retained discovery config and the
   entity appears automatically under the "AkiraOS <node>" device.

## Topic layout

`node` is `mqtt_service_node_id()` — `<CONFIG_AKIRA_MQTT_NODE_ID>_<hwid>`
(e.g. `akira_a1b2c3`).

| Purpose | Topic |
|---------|-------|
| Discovery config (retained) | `homeassistant/light/<node>_<object>/config` |
| State (retained) | `<node>/light/<object>/state` |
| Command | `<node>/light/<object>/set` |

## Writing an entity app

The `ha_rgb` app exposes the PWM RGB LED (GPIO4/5/6) as an HA light:

```c
#include "akira_api.h"   // manifest: "capabilities": ["mqtt","pwm"]

while (!mqtt_connected()) { delay(1000000); }
ha_light_register("rgb", "Akira RGB");            // discovery + subscribe
ha_light_report("rgb", 0, 255, 255, 255, 255);   // initial state

while (1) {
    int on, bri, r, g, b;
    int f = ha_light_poll("rgb", &on, &bri, &r, &g, &b, 2000);
    if (f > 0) {                 // bitmask of HA_CMD_STATE/BRIGHTNESS/COLOR
        drive_pwm(on, bri, r, g, b);
        ha_light_report("rgb", on, bri, r, g, b);
    }
}
```

For anything beyond a light, use the generic API: `mqtt_publish`,
`mqtt_subscribe`, `mqtt_poll`, `mqtt_connected` — publish your own discovery
config and handle your own topics.

## TLS

With `CONFIG_AKIRA_MQTT_TLS=y` the client connects on port 8883 and verifies the
broker against a CA certificate registered under TLS sec-tag
`CONFIG_AKIRA_MQTT_TLS_SEC_TAG` (default 42) via `tls_credential_add()`. Provision
the CA cert at boot (e.g. from a settings key or an embedded PEM) before the
first connect. Plaintext (1883) needs no cert.

## Testing

- `tests/src/test_mqtt_ha.c` unit-tests the discovery/state JSON builders and
  the command parser on `native_sim` (no broker needed).
- End-to-end: point `mqtt set` at a Mosquitto broker, `mqtt connect`, run
  `ha_rgb`, and toggle the light from Home Assistant.
