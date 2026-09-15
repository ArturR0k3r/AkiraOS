---
layout: default
title: Phone Companion App
parent: Architecture
nav_order: 12
---

# AkiraApp Phone Companion Integration Specification

> **Audience**: Mobile team building the AkiraApp companion application.  
> **Status**: **Specification draft**, not a description of shipped behaviour. The real
> companion service (`src/connectivity/bluetooth/companion_service.c`,
> `CONFIG_AKIRA_BT_COMPANION`, default `n`) was reworked after this document was written
> (2026-04-07) and has diverged from it. Treat the code as authoritative.

---

## Overview

AkiraApp is the companion phone application for AkiraConsole.  It communicates
with the device over **Bluetooth LE** using a custom GATT service implemented in
`src/connectivity/bluetooth/companion_service.c` and enabled by
`CONFIG_AKIRA_BT_COMPANION`.

### What the app can do

| Feature | Description |
|---------|-------------|
| Device info & status | Firmware version, free heap, running apps, BLE RSSI |
| App management | Install, start, stop, and uninstall WASM apps |
| Firmware OTA | Trigger a signed firmware update from a URL |
| Settings | Read and write device settings stored in NVS |
| Shell terminal | Execute Zephyr shell commands, stream output |
| File browser | List, download, upload, and delete files on `/lfs` |

### Platform recommendation

The mobile app should target **cross-platform** using either:

- **React Native** (recommended for web-team familiarity) with
  [`react-native-ble-plx`](https://github.com/dotintent/react-native-ble-plx) or
  [`react-native-bluetooth-le-manager`](https://github.com/innoveit/react-native-ble-manager)
- **Flutter** with [`flutter_blue_plus`](https://pub.dev/packages/flutter_blue_plus)

Both libraries expose BLE scanning, GATT connect/read/write/notify on iOS and
Android with a near-identical API surface.

---

## Mode Exclusivity

`BT_MODE_COMPANION` is **mutually exclusive** with `BT_MODE_HID` (keyboard/mouse)
and `BT_MODE_BLE_APP` (WASM BLE app).

Only one BLE mode can own the radio at a time (`CONFIG_BT_MAX_CONN = 1`).

The active mode is changed by the user from the device Settings screen or via the
shell command:
```
bt mode companion   # switch to companion mode
bt mode hid         # switch back to HID mode
bt mode none        # release BLE
```

The advertisement changes when the mode changes — the AkiraApp scan filter should
only show devices advertising the companion service UUID.

---

## BLE Connection Flow

```
Phone                          AkiraConsole
  |                                |
  |  Scan for UUID A1524C02-0001…  |
  |<——————————————————————————————— ADV_IND (custom UUID in AD)
  |                                |
  |  CONNECT_REQ                   |
  |———————————————————————————————>|
  |<——————————————————————————————— CONNECTION_ESTABLISHED
  |                                |
  |  MTU Exchange (request 247 B)  |
  |———————————————————————————————>|
  |<——————————————————————————————— MTU 247
  |                                |
  |  Discover Services             |
  |———————————————————————————————>|
  |<——————————————————————————————— Companion service + characteristics
  |                                |
  |  Enable CCCD on RESP_CHAR      |
  |———————————————————————————————>|
  |  Enable CCCD on DATA_DOWN      |
  |———————————————————————————————>|
  |  Enable CCCD on STATUS_CHAR    |
  |———————————————————————————————>|
  |<——————————————————————————————— STATUS_CHAR notify (device.info)
  |                                |
  |  ← Ready to send commands →   |
```

### MTU negotiation

Always request MTU **247** bytes.  This gives 244 bytes of ATT payload
(247 − 3 byte ATT header), which is the maximum characteristic write size
supported by the service.  The OS side accepts any negotiated MTU; larger MTU
reduces the number of DATA_DOWN chunks required for file and log transfers.

### Pairing & bonding

The device uses Bluetooth SMP (`CONFIG_BT_SMP=y`, `CONFIG_BT_BONDABLE=y`).  The
phone should initiate pairing after the first connection:

- **iOS**: pairing is triggered automatically when subscribing to a protected
  characteristic.  No explicit API call needed.
- **Android**: call `device.createBond()` after MTU exchange or handle the
  pairing intent from the BLE stack.

Once bonded the device stores the LTK in NVS.  Subsequent reconnections are
automatic without a new pairing ceremony.

To clear bonds: Settings → Bluetooth → Forget Device, or `bt unpair all` shell
command.

---

## GATT Service

### Service UUID

```
A1524C02-0001-4E56-8D4E-494B52413001
```

### Characteristics

| Name | UUID (base …3001) | Properties | Max size | Purpose |
|------|-------------------|-----------|---------|---------|
| `CMD_CHAR` | `…-0002-…` | WRITE | 244 B | JSON command (phone → device) |
| `RESP_CHAR` | `…-0003-…` | NOTIFY | 244 B | JSON response (device → phone) |
| `DATA_UP` | `…-0004-…` | WRITE_WITHOUT_RSP | 244 B | Bulk data phone → device |
| `DATA_DOWN` | `…-0005-…` | NOTIFY | 244 B | Bulk data device → phone |
| `STATUS_CHAR` | `…-0006-…` | READ + NOTIFY | 244 B | Periodic device status |

All 128-bit UUIDs share the base `A1524C02-XXXX-4E56-8D4E-494B52413001` where
`XXXX` is the 16-bit suffix shown above.

### Initialisation sequence

After connecting and negotiating MTU:

1. Enable **NOTIFY** on `RESP_CHAR` (CCC = `0x0001`)
2. Enable **NOTIFY** on `DATA_DOWN` (CCC = `0x0001`)
3. Enable **NOTIFY** on `STATUS_CHAR` (CCC = `0x0001`)
4. Device immediately pushes a `STATUS_CHAR` notification.

---

## Command Protocol

### Request format (CMD_CHAR WRITE)

```json
{ "op": "<operation>", "id": <integer>, "params": { ... } }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `op` | string | ✅ | Operation name (see table below) |
| `id` | integer | ✅ | Request ID — echoed in response |
| `params` | object | ✗ | Operation parameters |

### Response format (RESP_CHAR NOTIFY)

```json
{ "op": "<operation>", "id": <integer>, "ok": true,  "data": { ... } }
{ "op": "<operation>", "id": <integer>, "ok": false, "error": "<message>" }
```

### Operations reference

#### Device

| Op | Params | Response `data` | Description |
|----|--------|-----------------|-------------|
| `device.info` | — | `{fw, model, bt_addr}` | Firmware version and device info |
| `device.reboot` | — | — | Cold reboot |

```json
// Request
{ "op": "device.info", "id": 1 }

// Response
{ "op": "device.info", "id": 1, "ok": true,
  "data": { "fw": "1.4.8", "model": "AkiraConsole", "bt_addr": "AA:BB:CC:DD:EE:FF" } }
```

#### Apps

| Op | Params | Response `data` | Description |
|----|--------|-----------------|-------------|
| `apps.list` | — | `[{name, version, state}, …]` | List installed apps |
| `apps.start` | `{name}` | — | Launch app |
| `apps.stop` | `{name}` | — | Stop running app |
| `apps.uninstall` | `{name}` | — | Remove app |
| `apps.install.begin` | `{name, size}` | — | Prepare install transfer |
| `apps.install.end` | — | — | Commit and install |

```json
// List
{ "op": "apps.list", "id": 2 }
{ "op": "apps.list", "id": 2, "ok": true,
  "data": [
    { "name": "cube3d", "version": "2.1.0", "state": "running" },
    { "name": "gpio",   "version": "1.0.0", "state": "stopped" }
  ] }

// Install begin — data is sent over DATA_UP after this ACK
{ "op": "apps.install.begin", "id": 3, "params": { "name": "my_app", "size": 65536 } }
{ "op": "apps.install.begin", "id": 3, "ok": true }
```

#### Settings

| Op | Params | Response `data` | Description |
|----|--------|-----------------|-------------|
| `settings.list` | — | `[{key, value}, …]` | All settings |
| `settings.get` | `{key}` | `{key, value}` | Get one setting |
| `settings.set` | `{key, value}` | — | Write a setting |

```json
{ "op": "settings.set", "id": 10, "params": { "key": "display/brightness", "value": "80" } }
{ "op": "settings.set", "id": 10, "ok": true }
```

#### Shell terminal

| Op | Params | Response `data` | Description |
|----|--------|-----------------|-------------|
| `shell.exec` | `{cmd}` | `{output}` | Run a shell command |

Long-running commands stream output via `DATA_DOWN` `SHELL_OUT` frames while also
including the final tail in the response `output` field.

```json
{ "op": "shell.exec", "id": 20, "params": { "cmd": "kernel uptime" } }
{ "op": "shell.exec", "id": 20, "ok": true, "data": { "output": "Uptime: 300 s\n" } }
```

#### Files

| Op | Params | Response `data` | Description |
|----|--------|-----------------|-------------|
| `files.list` | `{path}` | `[{name, type, size}, …]` | Directory listing |
| `files.read` | `{path}` | — | Stream file over DATA_DOWN |
| `files.write` | `{path, size}` | — | Receive file over DATA_UP |
| `files.delete` | `{path}` | — | Delete file |
| `files.mkdir` | `{path}` | — | Create directory |

File access is limited to `/lfs/` (LittleFS partition).  Paths outside `/lfs`
are rejected with an error.

```json
{ "op": "files.list", "id": 30, "params": { "path": "/lfs/apps" } }
{ "op": "files.list", "id": 30, "ok": true,
  "data": [
    { "name": "hello_world", "type": "dir",  "size": 0    },
    { "name": "notes.txt",   "type": "file", "size": 1024 }
  ] }
```

#### OTA Firmware

| Op | Params | Response `data` | Description |
|----|--------|-----------------|-------------|
| `ota.start` | `{url, version, signature}` | — | Begin firmware update |
| `ota.status` | — | `{state, progress, version}` | Current OTA status |

The `signature` field is an Ed25519 signature (hex string) of the `.akfw` bundle.
The device fetches the firmware from `url` over HTTPS, verifying the signature
before handing it to the MCUboot OTA manager.

```json
{ "op": "ota.start", "id": 40, "params": {
    "url": "https://hub.akiraos.io/api/v1/firmware/1.5.0/download",
    "version": "1.5.0",
    "signature": "a1b2c3d4e5f6…"
} }
{ "op": "ota.start", "id": 40, "ok": true }
```

---

## Bulk Data Transfer (DATA_UP / DATA_DOWN)

Large payloads (app binaries, file contents, shell log streams) are fragmented
across multiple ATT writes using a 4-byte framing header on `DATA_UP` and
`DATA_DOWN`.

### Frame format

```
Byte 0:  Transfer type
           0x01  COMP_XFER_APP_DATA   — WASM binary chunk
           0x02  COMP_XFER_FILE_DATA  — file read/write chunk
           0x03  COMP_XFER_SHELL_OUT  — shell output line (DATA_DOWN only)

Byte 1:  Flags
           0x01  COMP_FLAG_LAST       — this is the final frame
           0x02  COMP_FLAG_ERROR      — sender is aborting the transfer

Byte 2:  Payload length LSB (little-endian uint16)
Byte 3:  Payload length MSB

Bytes 4…: Payload (up to 240 bytes)
```

### Sending an app binary (phone → device)

```
1.  Write CMD_CHAR:  { "op": "apps.install.begin", "id": 1,
                       "params": { "name": "my_app", "size": 65536 } }
2.  Await RESP_CHAR: { "ok": true }
3.  Split binary into 240-byte chunks.
4.  For each chunk i (0-based):
      frame[0] = 0x01                      // COMP_XFER_APP_DATA
      frame[1] = (i == last) ? 0x01 : 0x00 // COMP_FLAG_LAST on final chunk
      frame[2] = len & 0xFF
      frame[3] = len >> 8
      frame[4…] = chunk_bytes
      Write DATA_UP (WRITE_WITHOUT_RSP)
      Sleep 20 ms between chunks (device flash write time)
5.  Write CMD_CHAR:  { "op": "apps.install.end", "id": 2 }
6.  Await RESP_CHAR: { "ok": true }    ← install complete
```

### Receiving a file (device → phone)

```
1.  Write CMD_CHAR:  { "op": "files.read", "id": 1,
                       "params": { "path": "/lfs/notes.txt" } }
2.  Await RESP_CHAR: { "ok": true }     ← device starts streaming
3.  Receive DATA_DOWN notifications:
      frame[0] = 0x02                    // COMP_XFER_FILE_DATA
      frame[1] = flags                   // 0x01 = last chunk
      len = frame[2] | (frame[3] << 8)
      data = frame[4 … 4+len-1]
    Append data to buffer until COMP_FLAG_LAST is set.
```

---

## STATUS_CHAR — Periodic Device Status

The device pushes a JSON notification to `STATUS_CHAR` every 60 seconds (default)
and also immediately when:
- A phone subscribes to the characteristic
- An app starts or stops
- An OTA update completes

### Payload format

```json
{
  "fw": "1.4.8",
  "free_heap": 131072,
  "running_apps": ["cube3d"],
  "bt_rssi": -62
}
```

| Field | Type | Description |
|-------|------|-------------|
| `fw` | string | Firmware version |
| `free_heap` | integer | Free kernel heap bytes |
| `running_apps` | string[] | Names of currently running WASM apps |
| `bt_rssi` | integer | BLE RSSI of the current connection (dBm) |

---

## Error Codes

All error responses have `"ok": false` and an `"error"` string.

| Error string | Meaning |
|---|---|
| `"missing name"` | Required parameter absent |
| `"missing path"` | Required path parameter absent |
| `"not found"` | Requested app/file/setting does not exist |
| `"out of memory"` | Heap exhausted — reduce transfer size or free heap |
| `"invalid size"` | Transfer size zero or exceeds `CONFIG_AKIRA_BT_COMPANION_MAX_TRANSFER_KB` |
| `"storage error"` | LittleFS write or open failed |
| `"install failed: <n>"` | `app_manager_install_from_path()` returned error `n` |
| `"ota start failed: <n>"` | `ota_manager_start()` returned error `n` |
| `"no active transfer"` | `apps.install.end` sent without prior `apps.install.begin` |
| `"unknown op: <op>"` | Unrecognised operation string |

---

## BLE Advertising Packet

When in companion mode the device advertises:

```
AD type 0x01 (Flags):      0x06 (LE General Discoverable, BR/EDR Not Supported)
AD type 0x07 (128-bit UUIDs):  A1524C02-0001-4E56-8D4E-494B52413001
AD type 0x09 (Complete Name): "AkiraOS"
```

Scan filter for the app:
- Filter by service UUID `A1524C02-0001-4E56-8D4E-494B52413001`

The device name includes the last 4 digits of the BLE MAC when
`CONFIG_BT_DEVICE_NAME_DYNAMIC=y` is set — so you see `AkiraOS-AABB` in the scan
list, which helps users identify multiple consoles.

---

## Kconfig Reference

| Config option | Default | Description |
|---|---|---|
| `CONFIG_AKIRA_BT_COMPANION` | `n` | Enable companion service |
| `CONFIG_AKIRA_BT_COMPANION_STATUS_INTERVAL_MS` | `60000` | STATUS_CHAR notify interval |
| `CONFIG_AKIRA_BT_COMPANION_MAX_TRANSFER_KB` | `512` | Max single file/app transfer |
| `CONFIG_BT_L2CAP_TX_MTU` | `300` | L2CAP MTU — must be ≥ 247 |
| `CONFIG_BT_SMP` | `y` | Bluetooth security (pairing) |
| `CONFIG_BT_BONDABLE` | `y` | Store pairing keys in NVS |

---

## Self-Test Checklist

Before shipping the companion service integration, verify:

### OS side (automated)
- [ ] `west build -b akiraconsole_esp32s3_procpu` with `CONFIG_AKIRA_BT_COMPANION=y` compiles clean
- [ ] `west build -b native_sim` with `CONFIG_AKIRA_BT_COMPANION=n` compiles clean (config gating)
- [ ] SYS_INIT hook starts advertising with companion UUID at boot
- [ ] CMD_CHAR WRITE triggers RESP_CHAR notification within 500 ms

### App side (manual BLE smoke tests)
- [ ] Device appears in scan filtered by companion service UUID
- [ ] Connect + MTU 247 negotiation succeeds
- [ ] Pairing completes (iOS + Android)
- [ ] `device.info` op returns correct firmware version
- [ ] `apps.list` returns installed apps
- [ ] Install a 50 kB `.akpkg` via DATA_UP — `apps.install.end` returns `ok:true`
- [ ] `settings.set` / `settings.get` round-trip with same value
- [ ] `shell.exec` `{"cmd":"akira status"}` returns non-empty output
- [ ] `files.list` `{"path":"/lfs"}` returns directory contents
- [ ] `files.read` and `files.write` round-trip a 2 kB file
- [ ] STATUS_CHAR notification arrives within 65 seconds of subscribing
- [ ] `ota.status` returns valid state JSON

---

## Minimum Viable Integration Checklist

For the initial AkiraApp release the mobile team must implement:

- [ ] BLE scan filtered to companion service UUID, scan result shows device name
- [ ] Connect → MTU exchange → CCCD subscribe on RESP/DATA_DOWN/STATUS
- [ ] `device.info` call on connect — display firmware version in app
- [ ] STATUS_CHAR handler — update device health card in real-time
- [ ] App list screen — `apps.list`, start/stop/uninstall buttons
- [ ] App install screen — file picker → DATA_UP chunked transfer
- [ ] OTA update screen — `ota.start` with URL + signature from AkiraHub API
- [ ] Settings editor — `settings.list`, edit, `settings.set`
- [ ] Shell terminal screen — `shell.exec`, display output
- [ ] File browser — `files.list`, download (`files.read`), delete (`files.delete`)

---

*Last updated: 2026-09-14 (AkiraOS v1.6.5)*
