# Matter/Thread Feasibility Spike — ESP32-C6

**Duration:** 2 weeks
**Target:** ESP32-C6 (IEEE 802.15.4 + BLE 5.3 combo SoC)
**Date:** 2026-06-11
**Decision:** Implemented — co-processor IPC bridge shipped in v1.6

---

## Objective

Determine whether AkiraOS can act as the application OS while running Matter
(over Thread) on the same ESP32-C6, and expose a WASM bridge API so AkiraOS
apps can interact with the Matter fabric without direct native access.

---

## Findings

### RAM Constraint

`esp-matter` (Espressif's Matter SDK) requires **800 KB+ RAM** in its default
configuration. The ESP32-C6 has 512 KB of on-chip SRAM (plus optional PSRAM
on some modules). A direct port into the AkiraOS process model is **not
feasible** on most production targets without external PSRAM.

| Configuration | RAM Budget | Verdict |
|--------------|-----------|---------|
| AkiraOS alone (native_sim baseline) | ~180 KB | OK |
| AkiraOS + WAMR JIT | ~320 KB | Tight |
| AkiraOS + esp-matter (direct) | >800 KB | Infeasible on C6 without PSRAM |
| AkiraOS (app OS) + Matter (co-processor) | ~320 KB + co-proc RAM | Viable |

### Co-Processor Architecture

AkiraOS runs on the ESP32-C6 HP core (Zephyr). A Thread/Matter co-processor
(e.g. ESP32-H2 flashed with `esp-matter`) connects via **UART1** (TX=GPIO4,
RX=GPIO5). The co-processor owns the full Matter stack; AkiraOS sends commands
and receives events over a lightweight TLV frame protocol.

```
┌─────────────────────────────┐         ┌──────────────────────┐
│  ESP32-C6 HP core            │  UART1  │  ESP32-H2 (or equiv) │
│  AkiraOS (Zephyr)            │◄───────►│  esp-matter          │
│  WASM runtime                │ 115200  │  Thread/Matter stack │
│  akira_matter_ipc.c          │  baud   │  IEEE 802.15.4 radio │
└─────────────────────────────┘         └──────────────────────┘
```

### Frame Protocol

```
[0xAC][0xCE][CMD:1][SEQ:1][LEN_H:1][LEN_L:1][PAYLOAD:LEN][CRC16_H:1][CRC16_L:1]
```

| CMD | Direction | Purpose |
|-----|-----------|---------|
| 0x01 / 0x81 | host→coproc / resp | Commission device (controller) |
| 0x02 / 0x82 | host→coproc / resp | Send payload to device (controller) |
| 0x03 / 0x83 | host→coproc / resp | Subscribe to attribute (controller) |
| 0x04 | coproc→host | Async attribute event (controller) |
| 0x05 / 0x85 | host→coproc / resp | Status ping |
| 0x06 / 0x86 | host→coproc / resp | Register local endpoint (accessory) |
| 0x07 / 0x87 | host→coproc / resp | Report local attribute (accessory) |
| 0x08 / 0x88 | host→coproc / resp | Open local pairing window (accessory) |
| 0x09 / 0x89 | host→coproc / resp | Get this node's onboarding codes (accessory) |
| 0x0A | coproc→host | Inbound command to a local endpoint (accessory) |

CRC-16/IBM (poly 0x8005, reflect in+out, xorout 0xFFFF).
All response payloads begin with a 4-byte big-endian int32 status.

### Accessory-direction payload contract

This is the contract the co-processor firmware (or a mock) must honour so
AkiraOS can be adopted **as a Matter device** by Home Assistant, Google Home,
Alexa, and Apple Home. All multi-byte fields are big-endian.

| CMD | Request payload | Response payload (after status:4) |
|-----|-----------------|-----------------------------------|
| 0x06 EP_ADD | `device_type:2, n_clusters:1, cluster:4 × n` | `endpoint_id:1` |
| 0x07 ATTR_REPORT | `endpoint:1, cluster:4, attr:4, len:1, value:len` | — |
| 0x08 PAIR_OPEN | `timeout_sec:2` | — |
| 0x09 QR_GET | — | `qr:cstr\0, manual:cstr\0` |
| 0x0A ACC_CMD_EVENT | `endpoint:1, cluster:4, cmd:4, value:N` (coproc→host, unsolicited) | — |

The co-processor maps registered endpoints/clusters onto its `esp-matter`
data model, forwards `ATTR_REPORT` values into the fabric, and emits
`ACC_CMD_EVENT` when a controller invokes a command (e.g. OnOff/On) on one of
our endpoints. A reference implementation lives in
`tools/matter-coproc-mock/mock_coproc.py` and, for on-target testing, in
`src/connectivity/matter/matter_coproc_mock.c`
(`CONFIG_AKIRA_MATTER_COPROC_MOCK=y`).

---

## Implementation

### New files

| File | Description |
|------|-------------|
| `src/runtime/akira_matter_ipc.h` | IPC transport interface |
| `src/runtime/akira_matter_ipc.c` | UART framing + RX state machine thread |
| `src/api/akira_matter_api.h` | WASM native API declarations |
| `src/api/akira_matter_api.c` | Capability-guarded WASM bridge |

### Modified files

| File | Change |
|------|--------|
| `src/runtime/security.h` | `AKIRA_CAP_MATTER = 1ULL << 33` |
| `src/runtime/security.c` | `"matter"` / `"matter.*"` string mappings |
| `src/api/akira_export_api.c` | Register 4 native symbols under `CONFIG_AKIRA_WASM_MATTER` |
| `AkiraSDK/include/akira_api.h` | Public SDK declarations for WASM apps |
| `Kconfig` | `CONFIG_AKIRA_MATTER_COPROC_UART` + `CONFIG_AKIRA_WASM_MATTER` |
| `boards/esp32c6_devkitc_esp32c6_hpcore.conf` | `CONFIG_AKIRA_MATTER=n` (opt-in) |
| `boards/esp32c6_devkitc_esp32c6_hpcore.overlay` | UART1 node + `matter-coproc-uart` alias |

### WASM API

```c
// Manifest: "capabilities": ["matter"]

// Commission a device (BLE commissioning via co-processor)
int matter_commission(const char *passcode, void *eui64_out);

// Send a raw payload to a commissioned device
int matter_send(const void *eui64, const void *payload, int len);

// Subscribe to attribute change events
int matter_subscribe(const void *eui64, int attr_id);

// Poll for the next incoming event (blocking)
int matter_poll(void *src_eui64, int *attr_id, void *buf, int buf_len, int timeout_ms);
```

---

## Accessory mode (device-as-endpoint)

Enabled with `CONFIG_AKIRA_MATTER_ACCESSORY=y`. Lets AkiraOS expose **its own**
hardware as a Matter device that Home Assistant / Google Home / Alexa / Apple
Home adopt. The entity/cluster modelling lives in a **WASM app**; AkiraOS only
marshals it to the co-processor. Reference app:
`AkiraSDK/wasm_apps/generic/matter_rgb/` (PWM RGB → Extended Color Light).

```c
// Manifest: "capabilities": ["matter"]

// Register a local endpoint (device type + server clusters) -> endpoint id
int matter_endpoint_add(int device_type, const unsigned int *clusters, int n_clusters);

// Report a local attribute value outward to the fabric
int matter_report_attr(int endpoint, int cluster, int attr, const void *val, int len);

// Poll for the next inbound command targeting a local endpoint (blocking)
int matter_cmd_poll(int *endpoint, int *cluster, int *cmd, void *buf, int buf_len, int timeout_ms);

// Open this node's commissioning window so a controller can adopt it
int matter_open_pairing(int timeout_sec);

// Fetch this node's onboarding payload (QR string + manual pairing code)
int matter_get_pairing(char *qr, int qr_len, char *manual, int manual_len);
```

On-device, `matter pair` / `matter status` shell commands drive the same path.

**Testing without hardware:** build with `CONFIG_AKIRA_MATTER_COPROC_MOCK=y`
for an in-firmware responder, or run `tools/matter-coproc-mock/mock_coproc.py`
against a real/PTY UART. See `tests/matter_accessory/` for the native_sim
integration test.

---

## Enabling on ESP32-C6

In `boards/esp32c6_devkitc_esp32c6_hpcore.conf` (or a Kconfig fragment):

```kconfig
CONFIG_AKIRA_MATTER=y
CONFIG_AKIRA_MATTER_COPROC_UART=y
CONFIG_AKIRA_WASM_MATTER=y
```

The UART1 node in the overlay is disabled by default; enabling
`CONFIG_AKIRA_MATTER_COPROC_UART=y` causes the Zephyr DTS fixup to
set `uart1` to `status = "okay"`.

Connect the co-processor:
- ESP32-C6 GPIO4 (TX) → ESP32-H2 GPIO4 (RX)
- ESP32-C6 GPIO5 (RX) → ESP32-H2 GPIO5 (TX)
- Common GND

---

## Conditions to Extend to Full Matter

- ESP32-H2 firmware flashed with `esp-matter` + the AkiraOS co-processor
  companion firmware (TBD: `tools/matter-coproc/`)
- Matter commissioner integration tested with Apple Home / Google Home
- OTA update path for the co-processor firmware (via `akira-hub` catalogue)
