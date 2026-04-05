# Manifest Format Specification

App manifest defines metadata, capabilities, and resource requirements for WASM applications.

## Format Options

### Embedded Manifest (Recommended)

Embed manifest as a WASM custom section (preferred method):

```wasm
(module
  ;; Custom section: .akira.manifest  (content must be valid JSON)
  (custom ".akira.manifest"
    (data "{\"name\":\"sensor_logger\",\"version\":\"1.2.0\",\"capabilities\":[\"sensor.read\",\"storage.write\",\"display.write\"],\"memory_quota\":81920}")
  )
  
  ;; Rest of WASM module...
)
```

> **Important:** The custom section content is parsed as **JSON**, not as key-value pairs. The embedded data must be a valid JSON object.

**Advantages:**
- Single file deployment
- Manifest travels with the binary
- No separate JSON file to manage

### External JSON Manifest (Legacy)

Separate `.json` file alongside `.wasm` file:

**sensor_logger.json:**
```json
{
  "name": "sensor_logger",
  "version": "1.2.0",
  "author": "AkiraOS Team",
  "capabilities": ["sensor.read", "storage.write", "display.write"],
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
  "capabilities": ["display.write", "input.read", "sensor.read", "rf.transceive"]
}
```

**Available Capabilities:**

| Capability | Grants Access To |
|------------|------------------|
| `display.write` | Screen rendering (all `display_*` functions) |
| `input.read` | Button/touch input events |
| `gpio.read` | `gpio_read()`, `gpio_configure()` |
| `gpio.write` | `gpio_write()`, `gpio_configure()` |
| `sensor.read` | All `sensor_read()` channels (IMU, temp, etc.) |
| `timer` | All `timer_*()` functions |
| `ble` | All `ble_*()` functions |
| `hid` | All `hid_*()` functions |
| `storage.read` | `storage_open(O_READ)`, `storage_list()` |
| `storage.write` | `storage_open(O_WRITE/APPEND)`, `storage_delete()` |
| `network.*` | All `net_*()` TCP/UDP socket functions |
| `ipc` | All `msg_*()` publish/subscribe functions |
| `app.control` | `app_start()`, `app_stop()`, `app_list()`, `app_get_status()`, `app_get_self_name()` |
| `app.switch` | `app_switch()` |
| `rf.transceive` | All `rf_*()` radio functions |
| `uart` | All `uart_*()` functions |
| `i2c` | All `i2c_*()` functions |
| `pwm` | All `pwm_*()` functions |
| `power.read` | `power_get_*()` |
| `power.control` | `power_set_*()`, `power_wake_*()` |
| `memory` | `mem_alloc()`, `mem_free()` |
| `app.info` | `app_get_status()`, `app_list()`, `app_get_self_name()` (read-only) |
| `input.write` | Inject synthetic input events |
| `adc` | `adc_read()`, `adc_read_mv()` |
| `wdt` | `wdt_pet()` |

`printf()` and `delay()` are always available and require no capability declaration.

**Example:**
```json
{
  "capabilities": ["display.write", "input.read"]
}
```

#### Capability Aliases & Wildcards

The following group aliases are recognized and expand to all capabilities of that subsystem:

| Alias | Equivalent To |
|-------|---------------|
| `display.*` | `display.write` |
| `input.*` | `input.read` + `input.write` |
| `sensor.*` | `sensor.read` |
| `rf.*` | `rf.transceive` |
| `storage.*` | `storage.read` + `storage.write` |
| `gpio.*` | `gpio.read` + `gpio.write` |
| `bt.*` | `ble` + `hid` |
| `hw.*` | `timer` + `uart` + `i2c` + `pwm` + `adc` + `wdt` |
| `power.*` | `power.read` + `power.control` |
| `*` | All capabilities |

**Legacy / alternate names also accepted:**

| Alias | Resolves To |
|-------|-------------|
| `bt.shell` | `ble` |
| `display.read` | `display.write` |
| `memory.alloc` | `memory` |

---

### `memory_quota` (Optional)

Per-app memory limit in bytes.

```json
{
  "memory_quota": 65536
}
```

**Default:** No limit (unlimited heap) — omitting this field or setting it to `0` disables quota enforcement entirely.
**Recommended maximum:** 128KB (131072 bytes)

> **Note:** The runtime applies quota enforcement only when `memory_quota > 0`. There is no enforced minimum or maximum; the parser accepts any integer value as-is. The previously documented defaults of 64KB / min 16KB / max 128KB are not enforced by the runtime.

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

> **Runtime note:** This field is parsed from the manifest but is not stored in the runtime manifest struct. It has no effect on app execution and is currently informational only.

---

### `author` (Optional)

Developer or organization name.

```json
{
  "author": "AkiraOS Team"
}
```

> **Runtime note:** This field is parsed from the manifest but is not stored in the runtime manifest struct. It has no effect on app execution.

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

> **Runtime note:** This field is currently parsed but silently ignored — it is not stored in the runtime manifest struct and has no effect on boot behavior.

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

> **Runtime note:** This field is parsed but silently ignored at runtime — it is not stored in the runtime manifest struct and currently has no effect on scheduling.

---

## Complete Examples

### Minimal App

```json
{
  "name": "hello_world",
  "version": "1.0.0",
  "capabilities": ["display.write"]
}
```

### Sensor Logger

```json
{
  "name": "sensor_logger",
  "version": "2.1.0",
  "author": "Akira Team",
  "description": "Logs temperature and humidity to file",
  "capabilities": ["sensor.read", "storage.write", "display.write"],
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
    "sensor.read",
    "network.*",
    "rf.transceive",
    "storage.read"
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
  "capabilities": ["display.write"],
  "memory_quota": 32768
}
```

---

## Capability Matrix

Apps can combine capabilities based on use case:

| Use Case | Capabilities | Memory Quota |
|----------|--------------|-------------|
| ADC sampler | `adc` | 16–32 KB |
| Display UI | `display.write`, `input.read` | 32–64 KB |
| Sensor monitor | `sensor.read`, `display.write` | 48–80 KB |
| Data logger | `sensor.read`, `storage.write` | 64–96 KB |
| RF beacon | `rf.transceive` | 32 KB |
| Network client | `network.*`, `sensor.read` | 96–128 KB |
| BLE peripheral | `ble`, `display.write` | 64 KB |
| HID device | `hid`, `gpio.read` | 32–64 KB |

---

## Manifest Loading Priority

1. **Embedded custom section** (`.akira.manifest`)
2. **External JSON** (`<app_name>.json`)
3. **No manifest** — app loads with zero capabilities (`cap_mask = 0`) and no memory quota limit

> **Note:** There is no built-in default-capability fallback. If neither source is found, `manifest_parse_with_fallback()` returns `-ENOENT` and the app is granted no capabilities.

---

## Validation Rules

Runtime validates manifests and rejects apps that:
- Exceed max name length (31 chars) — `name` is truncated to 31 characters
- Have malformed JSON — returns `-EINVAL`
- Provide a malformed `capabilities` value (not an array) — returns `-EINVAL`

> **Note:** Unknown capability strings (e.g. `"admin"`) are **not** rejected — `akira_capability_str_to_mask()` silently returns `0` for unrecognised strings, which are simply OR-ed into the mask with no effect. Invalid version strings and quota values outside any range are accepted without error.

---

## Security Considerations

### Principle of Least Privilege

Only request capabilities you actually use:

**Too broad (avoid):**
```json
{
  "capabilities": ["display.write", "input.read", "sensor.read", "rf.transceive", "storage.write", "network.*"]
}
```

**Minimal (preferred):**
```json
{
  "capabilities": ["display.write", "input.read"]
}
```

### Capability Auditing

Before installing an app, review its manifest:

```bash
# Extract manifest from WASM
wasm-objdump -x app.wasm | grep .akira.manifest

# Or check JSON
cat app.json
```

**Red flags:**
- `network_server` without clear need
- `storage.write` in display-only app
- Excessive memory quota

---

## Embedding Manifest in WASM

### Using wasm-tools

```bash
# Install wasm-tools
cargo install wasm-tools

# Add custom section
wasm-tools custom app.wasm --add-section .akira.manifest=manifest.txt
```

### Using WAT (WebAssembly Text Format)

```wat
(module
  (custom ".akira.manifest"
    (data "{\"name\":\"my_app\",\"version\":\"1.0.0\",\"capabilities\":[\"display.write\"]}")
  )
  
  (import "env" "display_clear" (func $display_clear (param i32) (result i32)))
  
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
