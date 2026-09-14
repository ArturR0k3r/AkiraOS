---
layout: default
title: Web Interface
parent: Architecture
nav_order: 13
---

# AkiraOS Web Interface Specification

> **Audience**: Web team building the hosted management UI (e.g. `lab.akiraos.io`).  
> **Status**: **Specification draft**, not a description of shipped behaviour. Covers the
> USB Web Serial transport and the WiFi REST API as designed on 2026-04-07; verify endpoints
> against `src/connectivity/http/` before relying on them.

---

## Overview

AkiraConsole exposes a management web interface through **two transports**:

| Transport | Technology | Use case |
|-----------|-----------|---------|
| **WiFi (existing)** | HTTP REST + WebSocket on port 8080 | Device on local network |
| **USB (new)** | CDC ACM serial + JSON-RPC 2.0 via Web Serial API | Direct cable connection, no WiFi required |

The hosted web app (`lab.akiraos.dev` or self-hosted) detects which transport is
available and uses USB-over-serial when WiFi is not configured, exactly as
Flipper Zero's `lab.flipper.net` handles its device.

The device-side USB transport is implemented in
`src/connectivity/usb/usb_cdc_serial.c` and enabled by
`CONFIG_AKIRA_USB_CDC_SERIAL`.

---

## Transport 1 — WiFi REST API (existing)

Base URL: `http://<device-ip>:8080/api`

When the device has joined a WiFi network the same REST endpoints are available
as documented below.  The web app discovers the device via mDNS
(`_akira._tcp.local`) as described in `HUB_INTEGRATION.md`.

Endpoints are shared with the USB JSON-RPC transport — see the
[Endpoint Reference](#endpoint-reference) section.

---

## Transport 2 — USB Web Serial (new)

### Browser requirements

- **Chrome 89+** or **Edge 89+** on Windows, macOS, Linux, ChromeOS.
- Firefox and Safari currently do not support the Web Serial API.
- User must click a button to grant port access (Web Serial permission model).

### Connecting

```javascript
// Request a port (shows browser permission dialog)
const port = await navigator.serial.requestPort();
await port.open({ baudRate: 115200 });

const encoder = new TextEncoderStream();
const decoder = new TextDecoderStream();

encoder.readable.pipeTo(port.writable);
port.readable.pipeTo(decoder.writable);

const writer = encoder.writable.getWriter();
const reader = decoder.readable.getReader();

// Detect the device by sending a status request
await writer.write(JSON.stringify({
  jsonrpc: "2.0", id: 1, method: "GET", path: "/api/status"
}) + "\n");
```

### Protocol — JSON-RPC 2.0 over serial

Every message is a **single-line JSON object** terminated by `\n` (LF).
Carriage returns (`\r`) are ignored so CRLF line endings work too.

#### Request format (browser → device)

```json
{ "jsonrpc": "2.0", "id": <integer>, "method": "<VERB>", "path": "<path>", "params": {} }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `jsonrpc` | `"2.0"` | ✅ | Protocol version |
| `id` | integer | ✅ | Request ID — echoed in response |
| `method` | string | ✅ | HTTP verb: `GET`, `POST` |
| `path` | string | ✅ | API path (see Endpoint Reference) |
| `params` | object | ✗ | Query parameters as a JSON object |

#### Response format (device → browser)

```json
{ "jsonrpc": "2.0", "id": <integer>, "status": <http-code>, "body": <json> }
```

#### Unsolicited event format (device → browser)

```json
{ "jsonrpc": "2.0", "id": null, "event": "<type>", "data": <json-or-string> }
```

| Event type | Trigger | `data` content |
|------------|---------|----------------|
| `log` | System log line | string |
| `status` | Device state change | device status object |

### Chunked binary upload (app install / firmware OTA)

Binary data (`.akpkg` or `.akfw` files) is sent as a sequence of base64-encoded
chunks.  The `chunk` and `total` fields control reassembly on the device.

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "method": "POST",
  "path": "/api/apps/install",
  "chunk": 1,
  "total": 5,
  "data": "<base64-encoded-bytes>"
}
```

- **`chunk`**: 1-based chunk index  
- **`total`**: total number of chunks  
- **`data`**: base64-encoded payload (≤ 3 072 raw bytes per chunk → ≤ 4 096 base64 chars)

The device responds `206 Partial Content` for each non-final chunk and `200 OK`
(or an error) when the last chunk has been installed.

Suggested chunk size: **2 048 bytes** of raw data per chunk to avoid exceeding
the line buffer (`LINE_BUF_SIZE = 1024` chars after base64 expansion is well
within 4 096 limit).

```javascript
async function uploadFile(writer, reader, filePath, arrayBuffer) {
  const CHUNK_BYTES = 2048;
  const bytes = new Uint8Array(arrayBuffer);
  const total = Math.ceil(bytes.length / CHUNK_BYTES);

  for (let i = 0; i < total; i++) {
    const slice = bytes.slice(i * CHUNK_BYTES, (i + 1) * CHUNK_BYTES);
    const b64 = btoa(String.fromCharCode(...slice));

    await writer.write(JSON.stringify({
      jsonrpc: "2.0",
      id: 100 + i,
      method: "POST",
      path: "/api/apps/install",
      chunk: i + 1,
      total,
      data: b64,
    }) + "\n");

    // Wait for 206/200 before sending the next chunk
    const { value } = await reader.read();
    const resp = JSON.parse(value);
    if (resp.status !== 206 && resp.status !== 200) {
      throw new Error(`Upload error at chunk ${i + 1}: ${JSON.stringify(resp)}`);
    }
  }
}
```

---

## Endpoint Reference

All endpoints are available over **both** WiFi HTTP and USB JSON-RPC.

### Device

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/status` | Device status (IP, uptime, free heap, firmware version) |
| `GET` | `/api/system` | Extended system info (board, PSRAM, CPU load) |
| `POST` | `/api/reboot` | Reboot device |
| `GET` | `/api/logs` | Recent log buffer (over WiFi); subscribe to `log` events over USB |

#### `GET /api/status` response

```json
{
  "fw_version": "1.4.8",
  "codename": "GL1TCH",
  "uptime_ms": 120000,
  "free_heap_bytes": 131072,
  "wifi_ip": "192.168.1.42",
  "wifi_rssi": -55,
  "bt_mode": "companion"
}
```

### Apps

| Method | Path | Params | Description |
|--------|------|--------|-------------|
| `GET` | `/api/apps/list` | — | List installed WASM apps |
| `POST` | `/api/apps/start` | `name=<app>` | Start an app |
| `POST` | `/api/apps/stop` | `name=<app>` | Stop a running app |
| `POST` | `/api/apps/uninstall` | `name=<app>` | Remove an app |
| `POST` | `/api/apps/install` | chunked binary | Install `.akpkg` from browser |

#### `GET /api/apps/list` response

```json
[
  { "name": "hello_world", "version": "1.0.0", "state": "stopped" },
  { "name": "cube3d",      "version": "2.1.0", "state": "running"  }
]
```

### OTA Firmware

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/ota/status` | OTA progress / last result |
| `POST` | `/api/ota/confirm` | Confirm pending image (MCUboot swap) |

### Shell

| Method | Path | Params | Description |
|--------|------|--------|-------------|
| `GET` | `/api/cmd` | `c=<command>` | Execute a Zephyr shell command, get stdout in response |

```json
// Request
{ "jsonrpc": "2.0", "id": 5, "method": "GET", "path": "/api/cmd", "params": { "c": "akira status" } }

// Response
{ "jsonrpc": "2.0", "id": 5, "status": 200, "body": { "output": "AkiraOS 1.4.8 running\n" } }
```

---

## WebSocket (WiFi transport only)

When connecting over WiFi the web app may upgrade the HTTP connection to a
WebSocket on port `8081` for real-time log streaming:

```
ws://<device-ip>:8081/ws
```

The device pushes newline-terminated log lines as text frames.

On the USB transport, subscribe to `log` unsolicited events instead.

---

## Device Discovery

### USB
After `navigator.serial.requestPort()` the site should send `GET /api/status`
to verify the device is an AkiraConsole.  Check `fw_version` in the response.

### WiFi / mDNS
```
Service:  _akira._tcp.local
Instance: AkiraConsole-<last4ofMAC>._akira._tcp.local
Port:     8080
TXT:      fw=1.4.8  api=v1
```

---

## Security Considerations

- **USB**: The Web Serial API requires explicit user permission; access is not
  granted silently.  The JSON-RPC handler rejects lines longer than 1 024 bytes.
  Direct WASM app upload via `POST /api/apps/install` is gated by
  `CONFIG_AKIRA_HTTP_DEV_UPLOAD` (default `y`; disable in production builds).

- **WiFi**: By default no authentication is required on the local REST API
  (`CONFIG_AKIRA_HTTP_NO_AUTH` is `y`, same as Flipper's local web UI).  For
  production deployments on shared networks, set `CONFIG_AKIRA_HTTP_NO_AUTH=n`
  and provide a token via `CONFIG_AKIRA_HTTP_UPLOAD_TOKEN`; clients then send
  `Authorization: Bearer <token>`.

- **OTA and app uploads**: App packages are verified against the Ed25519
  signature before installation even when transferred via USB or WiFi — the
  signature is required regardless of transport.

---

## Minimum Viable Web-App Checklist

For the initial `lab.akiraos.io` USB feature the web team must implement:

- [ ] Web Serial port picker + `GET /api/status` device detection
- [ ] Display device status panel (firmware, heap, running apps)
- [ ] App list view: list, start, stop, uninstall
- [ ] App install: file picker → chunked base64 upload → progress bar
- [ ] Shell terminal: type command → `GET /api/cmd` → display output
- [ ] OTA firmware upload (same chunked protocol as app install but targeting `/api/ota/upload`)
- [ ] Graceful fallback to WiFi HTTP when USB not available
- [ ] Unsolicited `log` event handler → append to terminal pane

---

*Last updated: 2026-09-14 (AkiraOS v1.6.4)*
