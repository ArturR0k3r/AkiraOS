# Security Model

AkiraOS implements a **multi-layered security architecture** combining WASM sandboxing with custom capability-based access control.

## Security Layers

### 1. WASM Sandboxing (Base Layer)

**Provided by:** WebAssembly specification + WAMR runtime

**Isolation:**
- Memory-safe execution (no buffer overflows)
- Type-safe function calls
- Bounded linear memory (64-128KB per app)
- Stack isolation (no arbitrary stack access)
- No direct hardware access

**Limitations of WASM alone:**
- Prevents memory corruption
- Cannot control which native APIs are accessible
- No resource usage limits beyond memory bounds
- All imported functions equally accessible without additional enforcement

### 2. Capability System (AkiraRuntime Layer)

**Purpose:** Fine-grained permission control for native API access.

**Capability Bits (36 total, 64-bit mask).** These are generated from
`src/runtime/security.h` — keep the two in sync:
```c
// Hardware access (bits 0-4)
#define AKIRA_CAP_DISPLAY_WRITE  (1ULL << 0)   // Screen rendering
#define AKIRA_CAP_INPUT_READ     (1ULL << 1)   // Button/touch input
#define AKIRA_CAP_INPUT_WRITE    (1ULL << 2)   // Input device control
#define AKIRA_CAP_SENSOR_READ    (1ULL << 3)   // IMU, temp, etc.
#define AKIRA_CAP_RF_TRANSCEIVE  (1ULL << 4)   // WiFi/LoRa send/recv

// Communication + storage (bits 5-8)
#define AKIRA_CAP_BLE            (1ULL << 5)   // BLE app service
#define AKIRA_CAP_STORAGE_READ   (1ULL << 6)   // File system read
#define AKIRA_CAP_STORAGE_WRITE  (1ULL << 7)   // File system write
#define AKIRA_CAP_NETWORK        (1ULL << 8)   // TCP/UDP/HTTP

// Peripherals (bits 9-14)
#define AKIRA_CAP_GPIO_READ      (1ULL << 9)   // GPIO input
#define AKIRA_CAP_GPIO_WRITE     (1ULL << 10)  // GPIO output
#define AKIRA_CAP_TIMER          (1ULL << 11)  // Timer APIs
#define AKIRA_CAP_UART           (1ULL << 12)  // Serial communication
#define AKIRA_CAP_I2C            (1ULL << 13)  // I2C bus access
#define AKIRA_CAP_PWM            (1ULL << 14)  // PWM output

// System & app control (bits 15-22)
#define AKIRA_CAP_HID            (1ULL << 15)  // HID devices (ELEVATED)
#define AKIRA_CAP_APP_CONTROL    (1ULL << 16)  // Start/stop apps (ELEVATED)
#define AKIRA_CAP_IPC            (1ULL << 17)  // Inter-process messaging
#define AKIRA_CAP_APP_SWITCH     (1ULL << 18)  // Handoff to another app
#define AKIRA_CAP_MEMORY         (1ULL << 19)  // Quota-enforced heap APIs
#define AKIRA_CAP_APP_INFO       (1ULL << 20)  // App status queries
#define AKIRA_CAP_POWER_READ     (1ULL << 21)  // Battery level queries
#define AKIRA_CAP_POWER_CTRL     (1ULL << 22)  // Sleep mode control (ELEVATED)

// Services & peripherals (bits 23-30)
#define AKIRA_CAP_SETTINGS       (1ULL << 23)  // NVS key-value settings
#define AKIRA_CAP_ADC            (1ULL << 24)  // ADC channel read
#define AKIRA_CAP_WDT            (1ULL << 25)  // Feed system watchdog
#define AKIRA_CAP_FS_READ        (1ULL << 26)  // Sandboxed FS read
#define AKIRA_CAP_FS_WRITE       (1ULL << 27)  // Sandboxed FS write
#define AKIRA_CAP_CRYPTO         (1ULL << 28)  // Hash/encrypt/random
#define AKIRA_CAP_RTC_READ       (1ULL << 29)  // RTC read
#define AKIRA_CAP_RTC_WRITE      (1ULL << 30)  // RTC write / alarm

// Elevated & connectivity (bits 31-35)
#define AKIRA_CAP_OTA_TRIGGER    (1ULL << 31)  // Trigger OTA (ELEVATED)
#define AKIRA_CAP_AIINFER        (1ULL << 32)  // TFLite Micro inference
#define AKIRA_CAP_MATTER         (1ULL << 33)  // Matter/Thread IPC bridge
#define AKIRA_CAP_WIFI_INJECT    (1ULL << 34)  // 802.11 frame injection (ELEVATED)
#define AKIRA_CAP_MQTT           (1ULL << 35)  // MQTT + Home Assistant
```

**Elevated Privilege Capabilities:**
The following capabilities grant significant system control and should **not** be granted to untrusted apps:
- `AKIRA_CAP_HID` - Can emulate keyboards/mice
- `AKIRA_CAP_APP_CONTROL` - Can terminate other apps
- `AKIRA_CAP_POWER_CTRL` - Can modify power state
- `AKIRA_CAP_OTA_TRIGGER` - Can trigger a firmware update
- `AKIRA_CAP_WIFI_INJECT` - Can inject raw 802.11 management frames (deauth/disassoc)

**Enforcement:**
```c
// Macro delegates to security subsystem
#define AKIRA_CHECK_CAP_INLINE(exec_env, capability) \
    akira_security_check_exec(exec_env, capability)

// Alternative macro for functions returning int
#define AKIRA_CHECK_CAP_OR_RETURN(exec_env, cap, ret) \
    do { \
        if (!akira_security_check_exec(exec_env, cap)) { \
            return (ret); \
        } \
    } while(0)

// Example: Display API
int akira_native_display_clear(wasm_exec_env_t env, uint32_t color) {
    AKIRA_CHECK_CAP_OR_RETURN(env, AKIRA_CAP_DISPLAY_WRITE, -EACCES);
    return platform_display_clear(color);
}
```

**Performance:** ~60ns overhead per native call (estimated, board-dependent)

### 3. Resource Quotas (Memory Layer)

**Per-App Memory Limits:**
Memory quotas are defined per-app in manifest files (no hardcoded system-wide defaults). Example manifest:
```json
{
  "name": "sensor_logger",
  "memory_quota": 65536
}
```

**Quota Enforcement:**
The runtime tracks memory usage per app and enforces manifest-defined limits:
```c
// Simplified enforcement concept
if (app->memory_used + size > app->memory_quota) {
    return NULL;  // Quota exceeded
}
```

**Benefits:**
- Prevents one app from exhausting all memory
- Protects system stability  
- Configurable per-app via manifest
- Boards with PSRAM (ESP32-S3) support larger quotas

### 4. Trust Levels & Sandbox Contexts

**Trust Level System:**
Apps are assigned trust levels that determine syscall filtering and capability enforcement:

```c
typedef enum {
    TRUST_LEVEL_KERNEL = 0,  // Full system access
    TRUST_LEVEL_SYSTEM = 1,  // System services
    TRUST_LEVEL_TRUSTED = 2, // Verified apps
    TRUST_LEVEL_USER = 3     // Untrusted apps (default)
} akira_trust_level_t;
```

**Sandbox Context:**
Each app runs in a sandbox with:
- Trust level assignment
- Syscall filtering based on trust level
- Rate limiting for sensitive operations
- Performance statistics tracking

### 5. Audit Logging

**Security Event Tracking:**
The runtime logs security-relevant events for debugging and threat detection:

**Audit Events:**
- `AUDIT_EVENT_SYSCALL_DENIED` - Blocked syscall attempt
- `AUDIT_EVENT_CAPABILITY_DENIED` - Permission denied
- `AUDIT_EVENT_INTEGRITY_FAIL` - Binary validation failed
- `AUDIT_EVENT_SIGNATURE_FAIL` - Invalid signature
- `AUDIT_EVENT_SIGNATURE_OK` - Valid signature verified
- `AUDIT_EVENT_APP_LOADED` - App successfully loaded
- `AUDIT_EVENT_APP_STARTED` - App execution began
- `AUDIT_EVENT_WATCHDOG_KILL` - App terminated by watchdog

**Configuration:**
`CONFIG_AKIRA_AUDIT_LOG_SIZE` - Ring buffer size for audit events

### 6. Flash Protection (Boot Layer)

**MCUboot Verified Boot:**
- RSA/ECDSA signature verification
- Image headers with version info
- Rollback protection (anti-downgrade)
- Secure boot chain

**Flash Layout:**
```
┌─────────────────────────────────────┐
│ MCUboot (64KB) - Bootloader         │
├─────────────────────────────────────┤
│ Primary Slot (3MB) - Active FW      │ ← Signature verified
├─────────────────────────────────────┤
│ Secondary Slot (3MB) - OTA staging  │ ← Write-only during OTA
├─────────────────────────────────────┤
│ FS Partition (2MB) - Apps + data    │ ← Read-only for apps
└─────────────────────────────────────┘
```

**App Storage:**
- WASM files stored in read-only partition
- Apps cannot modify their own code
- Tampering requires OTA firmware update

## Manifest Format

Apps declare required capabilities in an embedded WASM custom section.

**Embedded Manifest (Preferred):**
```wasm
;; Custom section in .wasm file. Must be valid JSON.
(custom ".akira.manifest"
  "{\"name\": \"sensor_logger\", \"version\": \"1.2.0\", \"capabilities\": [\"sensor\", \"storage.write\", \"display\"], \"memory_quota\": 81920}"
)
```

**Fallback JSON:**
```json
{
  "name": "sensor_logger",
  "version": "1.2.0",
  "capabilities": ["sensor.read", "storage.write", "display.write"],
  "memory_quota": 81920
}
```

**Capability String Namespace:**
Supported capability strings (30+ mappings):
- **Hardware:** `"display.write"`, `"input.read"`, `"input.write"`, `"sensor.read"`, `"gpio.read"`, `"gpio.write"`, `"i2c"`, `"uart"`, `"pwm"`
- **Communication:** `"rf"`, `"ble"`, `"network"`, `"hid"`
- **Storage:** `"storage.read"`, `"storage.write"`
- **System:** `"timer"`, `"memory"`, `"power.read"`, `"power.control"`, `"app.control"`, `"app.switch"`, `"app.info"`, `"ipc"`
- **Wildcards:** `"display.*"`, `"input.*"`, `"sensor.*"`, `"gpio.*"`, `"storage.*"`, `"hw.*"`, `"*"` (all capabilities)

**Manifest Parsing:**
1. Try to extract embedded `.akira.manifest` custom section and parse JSON.
2. If not found, look for `<app_name>.json` in same directory.
3. If neither found, use default minimal capabilities.

## Threat Model

### In-Scope Threats

| Threat | Mitigation |
|--------|------------|
| **Malicious WASM app** | Sandboxing + capabilities |
| **Memory exhaustion** | Per-app quotas |
| **Unauthorized peripheral access** | Capability enforcement |
| **Code injection** | WASM type safety |
| **Firmware tampering** | MCUboot signature verification |
| **Downgrade attacks** | Version anti-rollback |

### Out-of-Scope Threats

| Threat | Status |
|--------|--------|
| **Physical access attacks** | Hardware-dependent (no secure element) |
| **Side-channel attacks** | Not mitigated (timing, power analysis) |
| **Bootloader exploits** | Depends on MCUboot security |
| **WiFi/BLE stack bugs** | Depends on Zephyr security |

## Attack Surface

### Minimal Attack Surface

**Exposed Interfaces:**
- HTTP server (port 8080)
- WebSocket server (port 8081)
- Bluetooth GATT services
- Native API calls from WASM

**NOT Exposed:**
- Direct hardware access
- Kernel syscalls
- Flash write access (except OTA)
- Network stack internals

### Privilege Separation

```
┌─────────────────────────────────┐  Lowest Privilege
│ WASM Apps (Sandboxed)           │  - No direct HW access
│                                 │  - Capability-checked APIs
└─────────────────────────────────┘
         ↓ Native Bridge
┌─────────────────────────────────┐
│ AkiraRuntime (Restricted)       │  - API enforcement
│                                 │  - Quota management
└─────────────────────────────────┘
         ↓ HAL Layer
┌─────────────────────────────────┐
│ Connectivity Layer              │  - Protocol handlers
│                                 │  - Network I/O
└─────────────────────────────────┘
         ↓ System Calls
┌─────────────────────────────────┐
│ Zephyr Kernel (Full Privilege)  │  - Hardware control
│                                 │  - Memory management
└─────────────────────────────────┘  Highest Privilege
```

## Security Best Practices

### For App Developers

1. **Request Minimal Capabilities** - Only request what you need
2. **Validate Input** - Check all native API return values
3. **Handle Quota Limits** - Gracefully handle malloc failures
4. **No Secrets in Code** - Use secure storage APIs (future)
5. **Audit Dependencies** - Review third-party WASM libraries

### For System Administrators

1. **Review Manifests** - Check capabilities before installing apps
2. **Monitor Resource Usage** - Track memory consumption
3. **Update Firmware** - Apply OTA updates for security patches
4. **Limit Network Exposure** - Firewall HTTP/BLE if not needed
5. **Verify Signatures** - Only install signed apps (future)

## Known Limitations

1. **Limited Storage Isolation** - Per-app directories enforced, but no encryption
2. **Coarse Capabilities** - All sensors share `AKIRA_CAP_SENSOR_READ`
3. **No Network Isolation** - Apps share network stack
4. **Shared Address Space** - All apps run in same kernel context
5. **No Hardware Security Module** - No TPM/secure element integration


## Related Documentation

- [Architecture Overview](index.md)
- [Runtime Architecture](runtime.md)
- [Manifest Format](../api-reference/manifest-format.md)
- [OTA Updates](../development/ota-updates.md)
