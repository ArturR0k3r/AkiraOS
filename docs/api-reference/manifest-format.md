# Manifest Format Specification

App manifest defines metadata, capabilities, and resource requirements for WASM applications.

## Format Options

### Embedded Manifest (Recommended)

Embed manifest as a WASM custom section (preferred method):

```wasm
(module
  ;; Custom section: akira-manifest
  (custom "akira-manifest"
    (data "\
      name: sensor_logger\n\
      version: 1.2.0\n\
      capabilities: sensor,fs_write,display\n\
      memory_quota: 81920\n\
      description: Logs sensor data to file\n\
    ")
  )
  
  ;; Rest of WASM module...
)
```

**Advantages:**
- ✅ Single file deployment
- ✅ Manifest travels with code
- ✅ No separate JSON to manage

### External JSON Manifest (Legacy)

Separate `.json` file alongside `.wasm` file:

**sensor_logger.json:**
```json
{
  "name": "sensor_logger",
  "version": "1.2.0",
  "author": "AkiraOS Team",
  "capabilities": ["sensor", "fs_write", "display"],
  "memory_quota": 81920,
  "description": "Logs sensor data to file"
}
```

**File naming:** Must match WASM filename: `app.wasm` → `app.json`

---

## Field Reference

### `name` (Required)

Application identifier (alphanumeric + underscore).

```json
{
  "name": "my_app"
}
```

**Rules:**
- Max length: 31 characters
- Pattern: `[a-zA-Z0-9_]+`
- Unique per device

---

### `version` (Required)

Semantic version string.

```json
{
  "version": "1.2.3"
}
```

**Format:** `MAJOR.MINOR.PATCH`

---

### `capabilities` (Required)

Array of permission strings.

```json
{
  "capabilities": ["display", "input", "sensor", "rf"]
}
```

**Available Capabilities:**
| Capability | Grants Access To | Risk Level |
|------------|------------------|------------|
| `display` | Screen rendering | Low |
| `input` | Button/touch reading | Low |
| `sensor` | All sensors (IMU, temp, etc.) | Low |
| `rf` | WiFi/BT/LoRa send/receive | **Medium** |
| `fs_read` | File system read | **Medium** |
| `fs_write` | File system write | **High** |
| `network_client` | HTTP/TCP client | **High** |
| `network_server` | HTTP/TCP server | **High** |

**Special Capabilities (Auto-Granted):**
- `log` - Always available, no declaration needed
- `time` - Always available

**Example:**
```json
{
  "capabilities": [
    "display",     // Minimal display app
    "input"
  ]
}
```

---

### `memory_quota` (Optional)

Per-app memory limit in bytes.

```json
{
  "memory_quota": 65536
}
```

**Default:** 64KB (65536 bytes)  
**Maximum:** 128KB (131072 bytes)  
**Minimum:** 16KB (16384 bytes)

**Guidelines:**
- Simple apps: 32-64KB
- Medium apps: 64-96KB
- Complex apps: 96-128KB

**Exceeded Quota:** `malloc()` returns `NULL`

---

### `description` (Optional)

Human-readable app description.

```json
{
  "description": "Displays sensor data on screen"
}
```

Max length: 256 characters

---

### `author` (Optional)

Developer or organization name.

```json
{
  "author": "AkiraOS Team"
}
```

---

### `autostart` (Optional)

Auto-start app on boot.

```json
{
  "autostart": true
}
```

**Default:** `false`

**Note:** Only one app can have `autostart: true`

---

### `priority` (Optional)

Execution priority hint (future use).

```json
{
  "priority": 5
}
```

**Range:** 1 (lowest) to 10 (highest)  
**Default:** 5

---

## Complete Examples

### Minimal App

```json
{
  "name": "hello_world",
  "version": "1.0.0",
  "capabilities": ["display", "log"]
}
```

### Sensor Logger

```json
{
  "name": "sensor_logger",
  "version": "2.1.0",
  "author": "Akira Team",
  "description": "Logs temperature and humidity to file",
  "capabilities": ["sensor", "fs_write", "display"],
  "memory_quota": 81920,
  "autostart": false
}
```

### Network Gateway

```json
{
  "name": "iot_gateway",
  "version": "1.0.0",
  "author": "IoT Corp",
  "description": "Forwards sensor data to cloud",
  "capabilities": [
    "sensor",
    "network_client",
    "rf",
    "fs_read"
  ],
  "memory_quota": 131072,
  "priority": 8,
  "autostart": true
}
```

### Display-Only App

```json
{
  "name": "clock",
  "version": "1.0.0",
  "description": "Displays current time",
  "capabilities": ["display"],
  "memory_quota": 32768
}
```

---

## Capability Matrix

Apps can combine capabilities based on use case:

| Use Case | Capabilities | Memory Quota |
|----------|--------------|--------------|
| **Display-only UI** | `display`, `input` | 32-64KB |
| **Sensor Monitor** | `sensor`, `display` | 48-80KB |
| **Data Logger** | `sensor`, `fs_write` | 64-96KB |
| **RF Beacon** | `rf` | 32KB |
| **Network Client** | `network_client`, `sensor` | 96-128KB |
| **Gateway** | `sensor`, `rf`, `network_client` | 128KB |

---

## Manifest Loading Priority

1. **Embedded custom section** (`akira-manifest`)
2. **External JSON** (`<app_name>.json`)
3. **Default fallback** (minimal capabilities)

---

## Validation Rules

Runtime validates manifests and rejects apps that:
- Exceed max name length (31 chars)
- Request undefined capabilities
- Request quota > 128KB
- Have invalid version format
- Missing required fields

**Error Handling:**
```bash
uart:~$ wasm load /apps/bad_app.wasm
[ERR] Manifest validation failed: unknown capability 'admin'
[ERR] Failed to load app
```

---

## Security Considerations

### Principle of Least Privilege

Only request capabilities you actually use:

❌ **Bad:**
```json
{
  "capabilities": ["display", "input", "sensor", "rf", "fs_write", "network_client"]
}
```

✅ **Good:**
```json
{
  "capabilities": ["display", "input"]
}
```

### Capability Auditing

Before installing an app, review its manifest:

```bash
# Extract manifest from WASM
wasm-objdump -x app.wasm | grep akira-manifest

# Or check JSON
cat app.json
```

**Red flags:**
- `network_server` without clear need
- `fs_write` in display-only app
- Excessive memory quota

---

## Embedding Manifest in WASM

### Using wasm-tools

```bash
# Install wasm-tools
cargo install wasm-tools

# Add custom section
wasm-tools custom app.wasm --add-section akira-manifest=manifest.txt
```

### Using WAT (WebAssembly Text Format)

```wat
(module
  (custom "akira-manifest"
    (data "name: my_app\nversion: 1.0.0\ncapabilities: display\n")
  )
  
  (import "akira" "display_clear" (func $display_clear (param i32) (result i32)))
  
  (func (export "_start")
    i32.const 0
    call $display_clear
    drop
  )
)
```

Compile with:
```bash
wat2wasm app.wat -o app.wasm
```

---

## Related Documentation

- [Native API Reference](native-api.md) - Function capabilities
- [Security Model](../architecture/security.md) - Capability enforcement
- [Building Apps](../development/building-apps.md) - WASM compilation
- [First App Tutorial](../getting-started/first-app.md) - Example workflow
