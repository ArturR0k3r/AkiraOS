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
    HTTP->>Transport: transport_notify(WASM_APP, data, len)
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

---

## Firmware Update Flow (OTA)

### Network → Flash → MCUboot

```mermaid
sequenceDiagram
    participant Client as HTTP Client
    participant HTTP as HTTP Server
    participant OTA as OTA Manager
    participant Flash as Flash Driver
    participant MCU as MCUboot

    Client->>HTTP: POST /ota/upload
    HTTP->>OTA: ota_manager_start()
    OTA->>Flash: Open secondary slot
    
    loop For each chunk
        Client->>HTTP: Send data chunk
        HTTP->>OTA: write_chunk(data, len)
        OTA->>OTA: Buffer (4KB alignment)
        OTA->>Flash: Write aligned block
    end
    
    Client->>HTTP: Upload complete
    HTTP->>OTA: ota_manager_finalize()
    OTA->>OTA: Verify checksum
    OTA->>Flash: Mark pending
    OTA-->>HTTP: 200 OK
    HTTP-->>Client: OTA complete
    
    Note over Client,MCU: System Reboot
    
    MCU->>MCU: Verify signature
    MCU->>MCU: Swap images
    MCU->>MCU: Boot new firmware
```

**Improvements:**
- ✅ Direct flash writes (no message queue)
- ✅ 2 data copies (down from 4)
- ✅ <10s for 1.1MB firmware
- ✅ No 120s timeout issues

**Data Copies:** 2 (network buffer → HTTP buffer → flash buffer → flash)

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

**Performance:**
- Hash lookup: ~20ns
- Capability check: ~10ns (inline)
- HAL call: ~30ns
- **Total overhead:** ~60ns

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
  └─ akira_native_sensor_read()         [~60ns overhead]
      └─ platform_sensor_read()          [HAL layer]
          └─ i2c_burst_read()             [Zephyr driver]
              └─ Hardware I2C transaction [~500μs]
```

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

**Quota Limits:**
- Default: 64KB per app
- Maximum: 128KB per app
- Total pool: 256KB PSRAM

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
2. Capability check - `CAP_FS_WRITE` verified
3. Path validation - Restrict to `/data/<app_name>/`
4. LittleFS write - Wear leveling, journaling
5. Flash write - Sector erase + program

**Read Path:** Similar but checks `CAP_FS_READ`

---

## Data Flow Summary

| Flow | Source | Destination | Copies | Peak Memory | Latency |
|------|--------|-------------|--------|-------------|---------|
| **App Upload** | HTTP | File System | 2 | ~22KB | ~200ms (100KB) |
| **OTA Update** | HTTP | Flash | 2 | ~6KB | ~10s (1.1MB) |
| **Native Call** | WASM | Hardware | 0 | N/A | ~60ns |
| **BLE Input** | HID | WASM | 1 | 64B | <5ms |
| **Sensor Read** | I2C | WASM | 1 | ~16B | ~500μs |
| **File Write** | WASM | Flash | 2 | ~4KB | ~10ms |

---

## Optimization Opportunities

### Current Bottlenecks
1. **HTTP → FS:** 2 copies (network → HTTP → FS)
2. **WASM Loading:** File-based (need network streaming)
3. **Native Calls:** WAMR hash lookup (~20ns)

### Planned Improvements
- **Zero-copy networking:** Stream directly to PSRAM
- **Static jump table:** Remove hash lookup (<50ns calls)
- **Network streaming:** Load WASM directly from HTTP

See [Implementation Tasks](../../IMPLEMENTATION_TASKS.md) for details.

---

## Related Documentation

- [Architecture Overview](index.md)
- [Connectivity Layer](connectivity.md)
- [Runtime Architecture](runtime.md)
- [Performance Benchmarks](../resources/performance.md)
