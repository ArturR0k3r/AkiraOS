# AkiraHub Integration Specification

> **Audience**: Hub team building the cloud service / web app for AkiraConsole.  
> **Status**: Draft v1.0 — implements the OS Shell install flow.

---

## Overview

AkiraHub is the optional cloud companion service for AkiraConsole devices.  
It provides:

- A catalogue of signed WASM apps that users can browse and push to their console.
- Remote app management (install, update, uninstall) via WebSocket push.
- Firmware OTA orchestration.
- A device registry so one account can manage multiple consoles.

The device-side client is implemented in `src/connectivity/cloud/` and
enabled by `CONFIG_AKIRA_CLOUD_CLIENT`.

---

## Package Format — `.akpkg`

Every app distributed through AkiraHub is packaged as a **gzip-compressed
tar archive** with the `.akpkg` extension.

```
app.akpkg
├── app.wasm          # WASM binary (interpreter or AOT for target arch)
├── manifest.json     # App metadata (see below)
└── icon_32.bmp       # Optional — 32×32 BMP, shown in the launcher
```

### `manifest.json` schema

```json
{
  "name": "my_app",
  "version": "1.2.0",
  "description": "A short description shown in the launcher.",
  "author": "Your Name",
  "capabilities": ["display.write", "gpio.read"],
  "memory_quota": 65536,
  "entry": "main",
  "target_arch": "xtensa"
}
```

| Field          | Type     | Required | Description |
|----------------|----------|----------|-------------|
| `name`         | string   | ✅       | Unique app identifier (ASCII, no spaces) |
| `version`      | string   | ✅       | Semantic version `MAJOR.MINOR.PATCH` |
| `description`  | string   | ✗        | Max 128 chars, displayed in launcher |
| `author`       | string   | ✗        | Publisher name |
| `capabilities` | string[] | ✅       | Required capability list (see below) |
| `memory_quota` | integer  | ✅       | Max WASM heap in bytes |
| `entry`        | string   | ✅       | WASM export to call as `main()` |
| `target_arch`  | string   | ✗        | `xtensa`, `arm`, `native` or `generic` |

### Capability strings

| Capability string | Maps to Kconfig / security.h |
|-------------------|------------------------------|
| `display.write`   | `AKIRA_CAP_DISPLAY_WRITE`    |
| `gpio.read`       | `AKIRA_CAP_GPIO_READ`        |
| `gpio.write`      | `AKIRA_CAP_GPIO_WRITE`       |
| `storage.read`    | `AKIRA_CAP_STORAGE_READ`     |
| `storage.write`   | `AKIRA_CAP_STORAGE_WRITE`    |
| `network.use`     | `AKIRA_CAP_NETWORK`          |
| `ble.use`         | `AKIRA_CAP_BLE`              |
| `app.control`     | `AKIRA_CAP_APP_CONTROL`      |

---

## REST API

Base URL: `https://hub.akiraos.io/api/v1` (or self-hosted).

All endpoints require `Authorization: Bearer <device_token>` unless noted.

### App Catalogue

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/catalog` | List all published apps (paginated) |
| `GET` | `/catalog/{id}` | Get metadata for one app |
| `GET` | `/catalog/{id}/download` | Download the `.akpkg` file |
| `POST` | `/catalog/publish` | Publish a new app (developer only) |
| `PUT` | `/catalog/{id}` | Update app metadata or binary |
| `DELETE` | `/catalog/{id}` | Retract an app (developer only) |

#### `GET /catalog` response

```json
{
  "total": 12,
  "page": 1,
  "per_page": 20,
  "apps": [
    {
      "id": "hello_world",
      "name": "Hello World",
      "version": "1.0.0",
      "description": "Minimal demo app.",
      "author": "AkiraOS Team",
      "size_bytes": 4096,
      "icon_url": "https://hub.akiraos.io/icons/hello_world.png",
      "published_at": "2025-01-15T12:00:00Z"
    }
  ]
}
```

#### `POST /catalog/publish` request

`Content-Type: multipart/form-data`

| Field    | Type | Description |
|----------|------|-------------|
| `package` | file | `.akpkg` archive |
| `signature` | string | Ed25519 signature of the package (hex) |

### Device Registry

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/devices/register` | Register a new device, returns `device_token` |
| `GET` | `/devices/me` | Get current device info |
| `GET` | `/devices/me/apps` | List apps installed on this device |
| `POST` | `/devices/me/apps/{id}/install` | Push install command |
| `DELETE` | `/devices/me/apps/{id}` | Push uninstall command |

### Firmware OTA

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/firmware/latest` | Get latest firmware metadata |
| `GET` | `/firmware/{version}/download` | Download signed firmware bundle |

---

## WebSocket Push Channel

The device keeps a persistent connection to:

```
wss://hub.akiraos.io/ws/device/{device_token}
```

### Server → Device messages

All messages are JSON with a `type` field.

#### `app.install`

```json
{
  "type": "app.install",
  "app_id": "hello_world",
  "version": "1.0.0",
  "download_url": "https://hub.akiraos.io/api/v1/catalog/hello_world/download"
}
```

The device downloads the `.akpkg`, verifies the Ed25519 signature, extracts
to `/lfs/apps/`, and calls `app_manager_install_from_path()`.

#### `app.uninstall`

```json
{
  "type": "app.uninstall",
  "app_id": "hello_world"
}
```

#### `firmware.update`

```json
{
  "type": "firmware.update",
  "version": "1.2.0",
  "download_url": "https://hub.akiraos.io/api/v1/firmware/1.2.0/download",
  "signature": "<ed25519-hex>"
}
```

### Device → Server messages

#### `device.status`

Sent on connect and every 60 seconds:

```json
{
  "type": "device.status",
  "firmware_version": "1.0.0",
  "free_heap_bytes": 131072,
  "running_apps": ["hello_world"],
  "wifi_rssi": -55
}
```

#### `app.install.result`

```json
{
  "type": "app.install.result",
  "app_id": "hello_world",
  "success": true,
  "error": null
}
```

---

## Authentication

### Device token

Each device generates a unique UUID v4 at first boot stored in NVS under
`akira/hub/device_token`.  The token is bound to the device's MAC address
via HMAC-SHA256 at registration time to prevent token theft.

```
HMAC-SHA256(key=wifi_mac_bytes, data=uuid_string)
```

### Package signature verification (Ed25519)

Each `.akpkg` is signed by the publisher's Ed25519 private key.  The device
verifies using the publisher public key embedded in the certificate chain
stored at `/lfs/hub/trusted_keys.pem` (updated via OTA bundles only).

Verification steps on device:
1. Compute SHA-256 of the raw `.akpkg` archive bytes.
2. Verify the Ed25519 signature provided in the HTTP response header
   `X-Akira-Signature: <hex>` or in the `POST /catalog/publish` form field.
3. Reject the package if verification fails — log at `LOG_ERR` level.

---

## Device Discovery

AkiraConsole advertises itself via **mDNS** when on a local network:

```
Service: _akira._tcp.local
Instance: AkiraConsole-<last4ofMAC>._akira._tcp.local
Port: 8080
TXT records:
  fw = 1.0.0
  api = v1
```

The Hub web app can use mDNS to discover consoles on the same LAN for
direct (cloud-bypass) installs.  The same REST API is exposed locally at
`http://<device-ip>:8080/api/v1`.

---

## Firmware Bundle Format

A signed firmware bundle (`.akfw`) is a binary file with the following
layout:

```
 Offset  Size  Field
 ------  ----  -----
      0     4  Magic: 0x414B4657 ("AKFW")
      4     2  Format version: 0x0001
      6    64  Ed25519 signature (signs bytes 70..end)
     70     4  Firmware image length (LE uint32)
     74     4  CRC32 of firmware image
     78     n  Raw firmware image (MCUboot slot0 binary)
```

The device checks magic, format version, Ed25519 signature, and CRC32 before
handing the image to `ota_manager`.

---

## Self-Hosted Hub

Hub is a stateless REST/WebSocket service.  Minimum stack:

- **HTTP server**: Any (nginx, Caddy, Node.js express, Go net/http).
- **WebSocket broker**: Any (socket.io, ws, gorilla/websocket).
- **Storage**: File system or S3-compatible object store for `.akpkg` files.
- **DB**: SQLite or PostgreSQL for device registry and catalogue metadata.

Environment variables:

| Variable | Description |
|----------|-------------|
| `AKIRA_HUB_SECRET` | HMAC secret for token validation |
| `AKIRA_HUB_ED25519_PRIVATE_KEY` | Publisher signing key (PEM) |
| `AKIRA_HUB_STORAGE_PATH` | Path to `.akpkg` file store |
| `AKIRA_HUB_PORT` | HTTP/WS listen port (default `8080`) |

---

## Minimum Viable Integration Checklist

For the initial AkiraConsole launch the Hub team must implement:

- [ ] `POST /devices/register` — device token issuance
- [ ] `GET /catalog` + `GET /catalog/{id}/download` — app browsing
- [ ] `POST /catalog/publish` — developer upload with Ed25519 signing
- [ ] WebSocket `/ws/device/{token}` — `app.install` + `app.uninstall` push
- [ ] mDNS `_akira._tcp.local` for local network discovery
- [ ] Ed25519 key distribution via `/lfs/hub/trusted_keys.pem` in OTA bundles
