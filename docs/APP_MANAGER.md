# AkiraOS App Manager

## Overview

The App Manager is a lightweight WebAssembly application management system built on top of OCRE (Open Container Runtime for Embedded). It provides installation, lifecycle management, and resource control for WASM applications on resource-constrained devices.

## System Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              EXTERNAL INTERFACES                             │
├─────────────┬─────────────┬─────────────┬─────────────┬─────────────────────┤
│   WiFi      │    BLE      │    USB      │  SD Card    │   Firmware Flash    │
│  (HTTP)     │  (GATT)     │   (MSC)     │   (SPI)     │     (Built-in)      │
└──────┬──────┴──────┬──────┴──────┬──────┴──────┬──────┴──────────┬──────────┘
       │             │             │             │                 │
       ▼             ▼             ▼             ▼                 ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           CONNECTIVITY LAYER                                 │
├─────────────────────┬───────────────────────┬───────────────────────────────┤
│   HTTP Server       │   BLE App Service     │   Storage Drivers             │
│   - POST /api/apps  │   - GATT Transfer     │   - USB Mount Handler         │
│   - Chunked Upload  │   - Chunked RX        │   - SD Card Mount             │
│   - Progress CB     │   - CRC32 Validate    │   - File Scanner              │
└─────────┬───────────┴───────────┬───────────┴───────────┬───────────────────┘
          │                       │                       │
          └───────────────────────┼───────────────────────┘
                                  ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                            APP MANAGER CORE                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────────────┐  │
│  │ Install Manager │  │  Core Registry  │  │    Lifecycle Manager        │  │
│  │                 │  │                 │  │                             │  │
│  │ • WASM Validate │  │ • App Metadata  │  │ • Start / Stop / Restart    │  │
│  │ • Magic Check   │  │ • State Machine │  │ • Crash Detection           │  │
│  │ • Size Verify   │  │ • Persistence   │  │ • Auto-restart (3 retries)  │  │
│  │ • Manifest Parse│  │ • ID Generation │  │ • Graceful Shutdown         │  │
│  │ • Flash Write   │  │ • Query/Filter  │  │ • Timeout Handling          │  │
│  └────────┬────────┘  └────────┬────────┘  └─────────────┬───────────────┘  │
│           │                    │                         │                   │
│           └────────────────────┼─────────────────────────┘                   │
│                                ▼                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                      Resource Quota Manager                          │   │
│  │                                                                      │   │
│  │   • Memory Limits (heap/stack per app)    • Max concurrent apps      │   │
│  │   • Execution time quotas                 • Storage quotas           │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────┬───────────────────────────────┘
                                              │
                                              ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                              OCRE RUNTIME                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                    WAMR (WebAssembly Micro Runtime)                 │    │
│  │                                                                     │    │
│  │   • AOT / Interpreter Mode        • WASI Subset (platform-dep)     │    │
│  │   • Sandboxed Execution           • Native API Bindings            │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────────┐   │
│  │  WASM App Slot 1 │  │  WASM App Slot 2 │  │   Native API Exports     │   │
│  │                  │  │                  │  │                          │   │
│  │  [sensor_app]    │  │  [data_logger]   │  │  • GPIO / I2C / SPI      │   │
│  │  Heap: 16KB      │  │  Heap: 16KB      │  │  • Sensor Read           │   │
│  │  Stack: 4KB      │  │  Stack: 4KB      │  │  • BLE Advertise         │   │
│  │  State: RUNNING  │  │  State: STOPPED  │  │  • HTTP Request          │   │
│  └──────────────────┘  └──────────────────┘  └──────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
                                              │
                                              ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                              STORAGE LAYER                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────┐  │
│  │   Internal Flash    │  │      SD Card        │  │    USB Storage      │  │
│  │     (LittleFS)      │  │      (FAT32)        │  │      (FAT32)        │  │
│  │                     │  │                     │  │                     │  │
│  │  /lfs/apps/         │  │  /sd/apps/          │  │  /usb/apps/         │  │
│  │  ├── registry.bin   │  │  ├── *.wasm         │  │  ├── *.wasm         │  │
│  │  ├── 001_app.wasm   │  │  └── *.json         │  │  └── *.json         │  │
│  │  └── 001_app.json   │  │                     │  │                     │  │
│  │                     │  │  (Source for        │  │  (Source for        │  │
│  │  /lfs/app_data/     │  │   installation)     │  │   installation)     │  │
│  │  └── <app_name>/    │  │                     │  │                     │  │
│  └─────────────────────┘  └─────────────────────┘  └─────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Connectivity Details

### WiFi / HTTP OTA

```
┌──────────────┐         ┌──────────────┐         ┌──────────────┐
│  Phone App   │  HTTP   │   AkiraOS    │  Write  │    Flash     │
│  or Browser  │ ──────► │  Web Server  │ ──────► │   Storage    │
└──────────────┘  POST   └──────────────┘         └──────────────┘
                         
Endpoint: POST /api/apps/install
Headers:  Content-Type: application/octet-stream
          X-App-Name: my_app (optional)
Body:     <raw WASM binary>
Response: {"id": 1, "name": "my_app", "status": "installed"}
```

### BLE Transfer Protocol

```
┌──────────────────────────────────────────────────────────────────┐
│                    BLE App Transfer Service                       │
│                    UUID: 0xAK01 (custom)                          │
├──────────────────────────────────────────────────────────────────┤
│                                                                   │
│  Characteristics:                                                 │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │ TX_CTRL (Write)     - Control commands                     │  │
│  │   0x01: Start transfer (+ total_size + name)               │  │
│  │   0x02: Chunk data (+ offset + data)                       │  │
│  │   0x03: End transfer (+ CRC32)                             │  │
│  │   0x04: Abort                                              │  │
│  └────────────────────────────────────────────────────────────┘  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │ RX_STATUS (Notify)  - Status responses                     │  │
│  │   0x00: Ready                                              │  │
│  │   0x01: Chunk ACK (+ next_offset)                          │  │
│  │   0x02: Complete                                           │  │
│  │   0xFF: Error (+ error_code)                               │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                   │
│  Transfer Flow:                                                   │
│  Phone ──[START]──► Device                                        │
│  Phone ◄──[READY]── Device                                        │
│  Phone ──[CHUNK]──► Device (repeat for each 512B chunk)           │
│  Phone ◄──[ACK]──── Device                                        │
│  Phone ──[END]────► Device                                        │
│  Phone ◄──[OK]───── Device                                        │
│                                                                   │
└──────────────────────────────────────────────────────────────────┘
```

### SD Card / USB Storage

```
┌─────────────────────────────────────────────────────────────────┐
│                    Storage Mount & Scan Flow                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   ┌─────────┐     ┌─────────────┐     ┌─────────────────────┐   │
│   │ SD/USB  │     │   Mount     │     │    App Scanner      │   │
│   │ Insert  │────►│  Handler    │────►│                     │   │
│   └─────────┘     └─────────────┘     │  1. List *.wasm     │   │
│                                       │  2. Check manifest  │   │
│   Events:                             │  3. Validate size   │   │
│   • Card inserted                     │  4. Queue install   │   │
│   • Card removed                      └──────────┬──────────┘   │
│   • Manual scan command                          │              │
│                                                  ▼              │
│                                       ┌─────────────────────┐   │
│   Shell Command:                      │  Install Manager    │   │
│   akira> app scan /sd                 │                     │   │
│   akira> app scan /usb                │  Copy to /lfs/apps/ │   │
│                                       └─────────────────────┘   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Firmware Preload

```
┌─────────────────────────────────────────────────────────────────┐
│                    Compile-Time App Embedding                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   Build System:                                                  │
│   ┌─────────────────────────────────────────────────────────┐   │
│   │  CMakeLists.txt                                         │   │
│   │                                                         │   │
│   │  # Embed WASM apps in firmware                          │   │
│   │  set(EMBEDDED_APPS                                      │   │
│   │      "${CMAKE_SOURCE_DIR}/apps/preload/sensor.wasm"     │   │
│   │      "${CMAKE_SOURCE_DIR}/apps/preload/logger.wasm"     │   │
│   │  )                                                      │   │
│   │  generate_inc_file_for_target(app EMBEDDED_APPS)        │   │
│   └─────────────────────────────────────────────────────────┘   │
│                                                                  │
│   Runtime:                                                       │
│   • Loaded from read-only flash at boot                          │
│   • Cannot be uninstalled (permanent)                            │
│   • Always available, survives factory reset                     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

## Constraints

| Parameter | Value | Notes |
|-----------|-------|-------|
| Max installed apps | 8-16 | Based on available flash size |
| Concurrent running | 1-2 | RAM constraints |
| Max app size | 64KB | Configurable per platform |
| Heap per app | 16KB | Default, adjustable in manifest |
| Stack per app | 4KB | Default, adjustable in manifest |

## App Sources Summary

| Source | Interface | Trigger | Notes |
|--------|-----------|---------|-------|
| HTTP OTA | WiFi | `POST /api/apps/install` | Chunked upload, progress callback |
| BLE Transfer | Bluetooth | GATT service 0xAK01 | 512B chunks, CRC32 validation |
| USB Storage | USB MSC | Mount event or `app scan /usb` | FAT32, manual confirmation |
| SD Card | SPI/SDIO | Boot scan or `app scan /sd` | FAT32, auto or manual install |
| Firmware | Flash | Boot | Read-only, cannot uninstall |

## App Manifest

**Optional but recommended.** Apps without manifest use defaults.

### Format: `app.json` (embedded or sidecar)

```json
{
  "name": "my_sensor_app",
  "version": "1.0.0",
  "entry": "_start",
  "memory": {
    "heap_kb": 16,
    "stack_kb": 4
  },
  "restart": {
    "enabled": true,
    "max_retries": 3,
    "delay_ms": 1000
  },
  "permissions": ["gpio", "sensor"]
}
```

### Defaults (no manifest)

| Field | Default |
|-------|---------|
| name | filename without extension |
| version | "0.0.0" |
| entry | "_start" |
| heap_kb | 16 |
| stack_kb | 4 |
| restart.enabled | false |
| restart.max_retries | 3 |
| permissions | none |

## App States

```
    ┌──────────────────────────────────────┐
    │                                      │
    ▼                                      │
┌────────┐   install   ┌───────────┐       │
│  NEW   │ ──────────► │ INSTALLED │       │
└────────┘             └─────┬─────┘       │
                             │ start       │
                             ▼             │
                       ┌───────────┐       │
              ┌─────── │  RUNNING  │ ◄─────┤ restart
              │        └─────┬─────┘       │ (if enabled)
              │ stop         │ crash       │
              ▼              ▼             │
        ┌───────────┐  ┌───────────┐       │
        │  STOPPED  │  │   ERROR   │ ──────┘
        └───────────┘  └───────────┘
                             │ max retries
                             ▼
                       ┌───────────┐
                       │  FAILED   │
                       └───────────┘
```

| State | Description |
|-------|-------------|
| NEW | Being installed, not yet ready |
| INSTALLED | Ready to run |
| RUNNING | Currently executing |
| STOPPED | Manually stopped |
| ERROR | Crashed, pending restart |
| FAILED | Exceeded max restart retries |

## Crash Handling

1. **On Crash:**
   - Log error with crash details
   - Set state to `ERROR`
   - Increment crash counter

2. **Auto-restart (if enabled):**
   - Wait `delay_ms` before restart
   - Retry up to `max_retries` times
   - After max retries → state = `FAILED`

3. **Recovery:**
   - Manual restart always allowed
   - `app restart <name>` resets crash counter

## Shell Commands

```
akira> app list
ID  NAME            VERSION  STATE      SIZE
1   sensor_reader   1.0.0    RUNNING    12KB
2   ble_beacon      0.2.1    STOPPED    8KB
3   data_logger     1.1.0    ERROR      15KB

akira> app install /sd/apps/myapp.wasm
Installing myapp... OK

akira> app start sensor_reader
Starting sensor_reader... OK

akira> app stop sensor_reader
Stopping sensor_reader... OK

akira> app restart data_logger
Restarting data_logger... OK (crash counter reset)

akira> app info sensor_reader
Name: sensor_reader
Version: 1.0.0
State: RUNNING
Size: 12KB
Heap: 16KB / Stack: 4KB
Crashes: 0
Auto-restart: enabled (max 3)

akira> app uninstall ble_beacon
Uninstall ble_beacon? [y/N]: y
Uninstalled.

akira> app scan /sd
Found 2 apps:
  /sd/apps/newapp.wasm (8KB)
  /sd/apps/demo.wasm (4KB)
```

## HTTP API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/apps` | GET | List all apps |
| `/api/apps` | POST | Install app (binary body) |
| `/api/apps/{id}` | GET | Get app info |
| `/api/apps/{id}` | DELETE | Uninstall app |
| `/api/apps/{id}/start` | POST | Start app |
| `/api/apps/{id}/stop` | POST | Stop app |
| `/api/apps/{id}/restart` | POST | Restart app |

## Storage Layout

```
/lfs/                          # LittleFS partition
├── apps/
│   ├── registry.bin           # App registry (metadata)
│   ├── 001_sensor_reader.wasm # App binary
│   ├── 001_sensor_reader.json # App manifest (optional)
│   ├── 002_ble_beacon.wasm
│   └── ...
└── app_data/
    ├── sensor_reader/         # Per-app persistent storage
    └── ble_beacon/

/sd/                           # SD Card (optional)
└── apps/
    ├── newapp.wasm            # Apps to install
    └── newapp.json            # Manifest (optional)
```

## Configuration

In `prj.conf`:

```kconfig
# App Manager
CONFIG_AKIRA_APP_MANAGER=y
CONFIG_AKIRA_APP_MAX_INSTALLED=8
CONFIG_AKIRA_APP_MAX_RUNNING=2
CONFIG_AKIRA_APP_MAX_SIZE_KB=64
CONFIG_AKIRA_APP_DEFAULT_HEAP_KB=16
CONFIG_AKIRA_APP_DEFAULT_STACK_KB=4

# App Sources
CONFIG_AKIRA_APP_SOURCE_HTTP=y
CONFIG_AKIRA_APP_SOURCE_BLE=y
CONFIG_AKIRA_APP_SOURCE_USB=y
CONFIG_AKIRA_APP_SOURCE_SD=y
CONFIG_AKIRA_APP_SOURCE_FIRMWARE=y

# Auto-restart defaults
CONFIG_AKIRA_APP_AUTO_RESTART=n
CONFIG_AKIRA_APP_MAX_RETRIES=3
CONFIG_AKIRA_APP_RESTART_DELAY_MS=1000
```

## Platform Support & Memory Configurations

The App Manager supports multiple platforms with optimized WAMR configurations:

### Full WAMR Mode (Default)
For platforms with sufficient memory (>512KB RAM or external PSRAM):

| Platform | Flash | RAM | WAMR Mode | Max Containers | Status |
|----------|-------|-----|-----------|----------------|--------|
| **ESP32-S3** (8MB PSRAM) | 11% | 85% DRAM + 59% PSRAM | Full | 4 | ✅ Recommended |
| **native_sim** | N/A | N/A | Full | 4 | ✅ Testing |

### Minimal WAMR Mode
For memory-constrained platforms (<512KB RAM), use the minimal WAMR build:

```kconfig
# Enable minimal WAMR for constrained devices
CONFIG_AKIRA_WAMR_MINI_LOADER=y
CONFIG_AKIRA_WAMR_MINIMAL=y

# Reduced memory settings
CONFIG_OCRE_WAMR_HEAP_BUFFER_SIZE=8192
CONFIG_OCRE_CONTAINER_DEFAULT_STACK_SIZE=2048
CONFIG_OCRE_CONTAINER_DEFAULT_HEAP_SIZE=8192
CONFIG_MAX_CONTAINERS=2
```

| Platform | Flash | RAM | WAMR Mode | Max Containers | Status |
|----------|-------|-----|-----------|----------------|--------|
| **STEVAL-STWINBX1** (STM32U585AI) | 92% | 32% | Minimal | 2 | ✅ Works |
| **nRF54L15 DK** | 62% | 75% | Minimal | 2 | ✅ Works |
| **ESP32-C3** | 21% | 74% | Minimal | 2 | ✅ Works |

### Minimal WAMR Features

When `CONFIG_AKIRA_WAMR_MINIMAL=y` is enabled, the following features are disabled to save memory:
- `bulk_memory` - Bulk memory operations
- `ref_types` - Reference types
- `multi_module` - Multiple WASM modules
- `tail_call` - Tail call optimization
- `simd` - SIMD instructions
- WASM logging

**Note:** Most simple WASM apps work fine with minimal mode. Only complex apps using advanced WASM features may need full mode.

### Platform-Specific Recommendations

| Platform | Max Apps | Concurrent | Recommended Use |
|----------|----------|------------|-----------------|
| ESP32-S3 | 16 | 4 | Full featured apps, sensor fusion |
| ESP32-C3 | 4 | 2 | Simple apps, WiFi gateway |
| nRF54L15 | 4 | 2 | BLE apps, low-power sensors |
| STM32U585 | 4 | 2 | Sensor apps, USB connectivity |
| native_sim | 16 | 4 | Development and testing |

## Future Enhancements

- [ ] App signing and verification
- [ ] Inter-app communication (IPC)
- [ ] App dependencies
- [ ] Remote app store
- [ ] App sandboxing with capabilities
