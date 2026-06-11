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
| 0x01 / 0x81 | host→coproc / resp | Commission device |
| 0x02 / 0x82 | host→coproc / resp | Send payload to device |
| 0x03 / 0x83 | host→coproc / resp | Subscribe to attribute |
| 0x04 | coproc→host | Async attribute event |
| 0x05 / 0x85 | host→coproc / resp | Status ping |

CRC-16/IBM (poly 0x8005, reflect in+out, xorout 0xFFFF).

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
