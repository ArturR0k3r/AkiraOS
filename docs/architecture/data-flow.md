---
layout: default
title: Data Flow
parent: Architecture
nav_order: 9
---

# Data Flow Architecture

End-to-end data flow diagrams showing how information moves through AkiraOS subsystems.

## Overview

Data flows through AkiraOS via three primary paths:
1. **Application Loading** - Network → Storage → Runtime
2. **Firmware Updates** - Network → Flash → MCUboot
3. **Runtime Execution** - WASM → Native APIs → Hardware

## Application Loading Flow

### Network Upload → File System → Runtime

```mermaid
sequenceDiagram
    participant Client as HTTP Client
    participant HTTP as HTTP Server
    participant Transport as Transport Interface
    participant Loader as App Loader
    participant FS as File System
    participant Runtime as AkiraRuntime
    participant WAMR as WAMR Engine

    Client->>HTTP: POST /upload (multipart)
    HTTP->>HTTP: Parse boundary
    HTTP->>Transport: transport_notify(TRANSPORT_DATA_WASM_APP, data, len)
    Transport->>Loader: callback(data, len)
    Loader->>FS: fs_write("/apps/new_app.wasm")
    FS-->>Loader: Written
    Loader-->>HTTP: 200 OK
    
    Note over Runtime,WAMR: Chunked Loading
    
    Runtime->>FS: fs_open("/apps/new_app.wasm")
    loop For each 16KB chunk
        Runtime->>FS: fs_read(16KB)
        Runtime->>WAMR: Feed chunk
    end
    WAMR->>WAMR: Parse & validate
    WAMR-->>Runtime: Module handle
    Runtime->>Runtime: Instantiate with caps
    Runtime-->>Client: App ready
```

**Data Copies:** 2 (network buffer → HTTP buffer → FS write buffer)

**Memory Usage:**
- HTTP buffer: 1.5KB
- FS write buffer: 4KB (internal)
- Runtime chunk: 16KB (temporary)
- Peak: ~22KB

**Transport Flags:**
- `TRANSPORT_FLAG_CHUNK_START` - Marks first chunk of transfer
- `TRANSPORT_FLAG_CHUNK_END` - Marks final chunk, triggers installation
- `TRANSPORT_FLAG_ABORT` - Cancels transfer, discards data

---

## Firmware Update Flow (OTA)

### Network → Transport → OTA Manager → Flash → MCUboot

```mermaid
sequenceDiagram
    participant Client as HTTP Client
    participant HTTP as HTTP Server
    participant Transport as Transport Interface
    participant OTA as OTA Manager
    participant Flash as Flash Driver
    participant MCU as MCUboot

    Client->>HTTP: POST /ota/upload
    HTTP->>Transport: transport_begin(FIRMWARE)
    Transport->>OTA: ota_data_callback(CHUNK_START)
    OTA->>Flash: Open secondary slot
    
    loop For each chunk
        Client->>HTTP: Send data chunk
        HTTP->>Transport: transport_notify(data, len)
        Transport->>OTA: ota_data_callback(data)
        OTA->>OTA: Buffer (4KB alignment)
        OTA->>Flash: Write aligned block
    end
    
    Client->>HTTP: Upload complete
    HTTP->>Transport: transport_notify(CHUNK_END)
    Transport->>OTA: ota_data_callback(CHUNK_END)
    OTA->>OTA: Validate image header
    OTA->>Flash: Mark pending (boot_request_upgrade)
    OTA-->>HTTP: 200 OK
    HTTP-->>Client: OTA complete
    
    Note over Client,MCU: System Reboot
    
    MCU->>MCU: Verify signature
    MCU->>MCU: Swap images
    MCU->>MCU: Boot new firmware
```

**Characteristics:**
- Callback-based via transport interface (no direct HTTP→OTA coupling)
- Direct flash writes (no message queue overhead)
- 4KB alignment buffering
- <10 s for 1.1 MB firmware
- Configurable socket timeout

**Data Copies:** 3 stages (network buffer → HTTP multipart parser → OTA alignment buffer → flash)

**Memory Usage:**
- HTTP buffer: 1.5KB
- OTA alignment buffer: 4KB
- Peak: ~6KB

---

## Runtime Execution Flow

### WASM Application → Native APIs → Hardware

```mermaid
sequenceDiagram
    participant WASM as WASM Code
    participant WAMR as WAMR Engine
    participant Bridge as Native Bridge
    participant Security as Security Layer
    participant HAL as Platform HAL
    participant HW as Hardware

    WASM->>WAMR: Call imported function
    WAMR->>WAMR: Hash table lookup
    WAMR->>Bridge: Native function stub
    Bridge->>Security: Inline capability check
    alt Has capability
        Security->>HAL: Forward call
        HAL->>HW: Hardware operation
        HW-->>HAL: Result
        HAL-->>Bridge: Success
        Bridge-->>WAMR: Return value
        WAMR-->>WASM: Result
    else No capability
        Security-->>Bridge: -EACCES
        Bridge-->>WAMR: Error
        WAMR-->>WASM: Permission denied
    end
```

**Performance (estimated):**
- Hash lookup: ~20ns (WAMR native function resolution)
- Capability check: ~10ns (inline bitmask check)
- HAL call: ~30ns (function call overhead)
- **Total overhead:** ~60ns

> **Note:** These are estimated values. Actual performance depends on hardware, compiler optimization, and cache behavior.

---

## Bluetooth Data Flow

### BLE → HID Manager → Runtime → WASM

```mermaid
sequenceDiagram
    participant Device as BLE Device
    participant BLE as BLE Stack
    participant HID as HID Manager
    participant Transport as Transport Interface
    participant Runtime as AkiraRuntime
    participant App as WASM App

    Device->>BLE: HID Report (64B)
    BLE->>HID: GATT characteristic update
    HID->>HID: Parse HID report
    HID->>Transport: transport_notify(INPUT, data, len)
    Transport->>Runtime: Dispatch to apps
    Runtime->>App: akira_native_input_read_buttons()
    App-->>Runtime: Process input
```

**Latency:** <5ms from BLE event to WASM callback

---

## Sensor Data Flow

### Sensor → Driver → WASM

```mermaid
graph LR
    classDef hw fill:#50C878,stroke:#fff,color:#fff
    classDef driver fill:#4A90E2,stroke:#fff,color:#fff
    classDef api fill:#E94B3C,stroke:#fff,color:#fff
    classDef wasm fill:#9B59B6,stroke:#fff,color:#fff

    SENSOR[IMU Sensor]:::hw
    I2C[I2C Driver]:::driver
    HAL[Sensor HAL]:::driver
    API[Native API]:::api
    WASM[WASM App]:::wasm

    SENSOR -->|I2C bus| I2C
    I2C --> HAL
    HAL --> API
    API --> WASM
```

**Call Stack:**
```
wasm_app_code()
  └─ akira_native_sensor_read()         [~60ns overhead*]
      └─ platform_sensor_read()          [HAL layer]
          └─ i2c_burst_read()             [Zephyr driver]
              └─ Hardware I2C transaction [~500μs*]
```

> **Note:** Performance metrics marked with * are estimates based on typical embedded system performance. Actual values may vary by hardware and configuration.

---

## Memory Allocation Flow

### WASM malloc → Quota Check → PSRAM

```mermaid
sequenceDiagram
    participant WASM as WASM Code
    participant WAMR as WAMR Libc
    participant Quota as Quota Manager
    participant PSRAM as PSRAM Heap

    WASM->>WAMR: malloc(size)
    WAMR->>Quota: akira_wasm_malloc(size)
    Quota->>Quota: Check: used + size <= quota?
    alt Within quota
        Quota->>PSRAM: psram_malloc(size)
        PSRAM-->>Quota: Pointer
        Quota->>Quota: memory_used += size
        Quota-->>WAMR: Pointer
        WAMR-->>WASM: Success
    else Quota exceeded
        Quota-->>WAMR: NULL
        WAMR-->>WASM: NULL (allocation failed)
    end
```

**Quota Configuration:**
- Per-instance heap: 64KB (WAMR default)
- Manifest-defined quota: Configurable per app (no hardcoded maximum)
- Total WAMR heap: 512KB default (configurable)
- Quota enforcement: Atomic tracking in `akira_wasm_malloc/free`

> **Note:** Memory quotas are specified in app manifests via `memory_quota` field. Apps without quotas use WAMR instance heap limits only.

---

## File System Operations

### WASM → FS API → LittleFS → Flash

```mermaid
graph TB
    classDef wasm fill:#9B59B6,stroke:#fff,color:#fff
    classDef api fill:#4A90E2,stroke:#fff,color:#fff
    classDef fs fill:#50C878,stroke:#fff,color:#fff
    classDef hw fill:#E94B3C,stroke:#fff,color:#fff

    WASM[WASM App]:::wasm
    FSAPI[FS Native API]:::api
    LITTLEFS[LittleFS]:::fs
    FLASH[Flash Driver]:::hw

    WASM -->|"fs_write()"| FSAPI
    FSAPI -->|Capability check| LITTLEFS
    LITTLEFS -->|Block write| FLASH
```

**Write Path:**
1. `wasm_app_write()` - WASM calls native FS API
2. Capability check - `AKIRA_CAP_STORAGE_WRITE` verified
3. Path sandboxing - Restrict to app-specific paths:
   - `/SD:/apps/<app_name>/` (SD card)
   - `/lfs/apps/<app_name>/` (LittleFS flash)
   - `/ram/apps/<app_name>/` (RAM filesystem)
4. Path traversal protection - Reject ".." sequences
5. LittleFS write - Wear leveling, journaling
6. Flash write - Sector erase + program

**Read Path:** Similar but checks `AKIRA_CAP_STORAGE_READ`

---

## Data Flow Summary

| Flow | Source | Destination | Copies | Peak Memory | Latency |
|------|--------|-------------|--------|-------------|---------|
| **App Upload** | HTTP | File System | 2 | ~22KB | ~200ms (100KB) |
| **OTA Update** | HTTP | Flash | 2 | ~6KB | ~10s (1.1MB) |
| **Native Call** | WASM | Hardware | 0 | N/A | ~60ns* |
| **HID Output** | WASM | BLE Host | 1 | 8-64B | ~5-10ms* |
| **Sensor Read** | I2C | WASM | 1 | ~16B | ~500μs* |
| **File Write** | WASM | Flash | 2 | ~4KB | ~10ms |

> **Note:** Latency values marked with * are estimates. Actual performance varies.

---

## Optimization Opportunities

### Current Bottlenecks
1. **HTTP → FS:** 2 copies (network → HTTP → FS)
2. **WASM Loading:** File-based (need network streaming)
3. **Native Calls:** WAMR hash lookup (~20ns estimated overhead)

### Planned Improvements
- **Zero-copy networking:** Stream directly to PSRAM
- **Static jump table:** Remove hash lookup (<50ns calls estimated)
- **Network streaming:** Load WASM directly from HTTP


---

## Related Documentation

- [Architecture Overview](index.md)
- [Connectivity Layer](connectivity.md)
- [Runtime Architecture](runtime.md)
- [Performance Benchmarks](../resources/performance.md)

---

*Last updated: 2026-09-14 (AkiraOS v1.6.4)*
