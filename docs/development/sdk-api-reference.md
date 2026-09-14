---
layout: default
title: SDK API Reference
parent: Development
nav_order: 4
---

# Akira SDK API Reference

> **The canonical API reference lives in the [AkiraSDK submodule](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/API_REFERENCE.md).**
>
> To avoid duplicating content that changes with every SDK release, the full API reference is maintained in AkiraSDK. Refer to it directly — the local copy in the submodule is always in sync.

## Where to Find the API Reference

| Location | Path |
|----------|------|
| **Online (canonical)** | [github.com/ArturR0k3r/AkiraSDK — API_REFERENCE.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/API_REFERENCE.md) |
| **Local submodule** | `AkiraSDK/docs/API_REFERENCE.md` |
| **Header file** | `AkiraSDK/include/akira_api.h` |

## Quick Start

Include the single SDK header in every app:

```c
#include "akira_api.h"
```

## API Groups

| Group | Capability | Key Functions |
|-------|-----------|---------------|
| **Display** | `display.write` | `display_clear`, `display_flush`, `display_text`, `display_rect`, `display_circle` |
| **GPIO** | `gpio.read` / `gpio.write` | `gpio_read`, `gpio_write`, `gpio_configure` |
| **Sensors** | `sensor.read` | `sensor_read(channel)` |
| **Timer** | `timer` | `timer_create`, `timer_start`, `timer_elapsed`, `delay` |
| **BLE** | `ble` | `ble_init`, `ble_service_create`, `ble_char_create`, `ble_advertise`, `ble_event_pop` |
| **HID** | `hid` | `hid_key_press`, `hid_key_release`, `hid_mouse_move`, `hid_gamepad_press`, `hid_consumer_send` |
| **Storage** | `storage.read` / `storage.write` | `storage_open`, `storage_read`, `storage_write`, `storage_close`, `storage_list` |
| **Networking** | `network.*` | `net_open`, `net_connect`, `net_close`, `net_tx_bind`, `net_rx_bind`, `net_event_pop` |
| **IPC** | `ipc` | `msg_publish`, `msg_subscribe`, `msg_recv`, `msg_try_recv` |
| **UART** | `uart` | `uart_open`, `uart_write`, `uart_read`, `uart_close` |
| **I2C** | `i2c` | `i2c_write_reg`, `i2c_read_reg` |
| **PWM** | `pwm` | `pwm_set` |
| **Lifecycle** | `app.control` | `app_start`, `app_stop`, `app_list` |
| **Memory** | _(always available)_ | `mem_alloc`, `mem_free` |
| **Logging** | _(always available)_ | `printf()` (from `akira_api.h`) |

## Related Documentation

- [Best Practices](best-practices.md) — Patterns for efficient, maintainable apps
- [SDK Troubleshooting](sdk-troubleshooting.md) — Debug common app issues
- [Building WASM Apps](building-apps.md) — Build toolchain and workflow
- [Manifest Format](../api-reference/manifest-format.md) — Capability declarations
- [Native API Reference](../api-reference/native-api.md) — Low-level OS-side API
- [Sample Apps](https://github.com/ArturR0k3r/AkiraSDK/tree/v1.6.x/wasm_apps) — Reference implementations

---

*Last updated: 2026-09-14 (AkiraOS v1.6.4)*
