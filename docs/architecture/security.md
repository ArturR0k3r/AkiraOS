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
- ✅ Prevents memory corruption
- ❌ Cannot control which native APIs are accessible
- ❌ No resource usage limits beyond memory
- ❌ All imported functions equally accessible

### 2. Capability System (AkiraRuntime Layer)

**Purpose:** Fine-grained permission control for native API access.

**Capability Bits:**
```c
#define CAP_DISPLAY_WRITE   (1U << 0)  // Screen rendering
#define CAP_INPUT_READ      (1U << 1)  // Button/touch input
#define CAP_SENSOR_READ     (1U << 2)  // IMU, temp, etc.
#define CAP_RF_TRANSCEIVE   (1U << 3)  // WiFi/BT/LoRa send/recv
#define CAP_FS_READ         (1U << 4)  // File system read
#define CAP_FS_WRITE        (1U << 5)  // File system write
#define CAP_NETWORK_CLIENT  (1U << 6)  // HTTP/TCP client
#define CAP_NETWORK_SERVER  (1U << 7)  // HTTP/TCP server
```

**Enforcement:**
```c
// Inline macro (fast path)
#define AKIRA_CHECK_CAP_INLINE(inst, cap) \
    do { \
        uint32_t mask = get_app_cap_mask(inst); \
        if (!(mask & cap)) return -EACCES; \
    } while(0)

// Example: Display API
int akira_native_display_clear(wasm_exec_env_t env, uint32_t color) {
    AKIRA_CHECK_CAP_INLINE(get_module_inst(env), CAP_DISPLAY_WRITE);
    return platform_display_clear(color);
}
```

**Performance:** ~60ns overhead per native call (inline check, no function call)

### 3. Resource Quotas (Memory Layer)

**Per-App Memory Limits:**
```c
#define DEFAULT_APP_QUOTA   (64 * 1024)   // 64KB default
#define MAX_APP_QUOTA      (128 * 1024)   // 128KB maximum
```

**Quota Enforcement:**
```c
void *akira_wasm_malloc(size_t size) {
    akira_managed_app_t *app = get_current_app();
    
    // Check quota
    if (app->memory_used + size > app->memory_quota) {
        LOG_ERR("App %s exceeded quota: %zu + %zu > %zu",
                app->name, app->memory_used, size, app->memory_quota);
        return NULL;
    }
    
    void *ptr = psram_malloc(size);
    if (ptr) {
        app->memory_used += size;
    }
    return ptr;
}
```

**Benefits:**
- Prevents one app from exhausting all memory
- Protects system stability
- Configurable per-app limits

### 4. Flash Protection (Boot Layer)

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
;; Custom section in .wasm file
(custom "akira-manifest"
  (name "sensor_logger")
  (version "1.2.0")
  (author "AkiraOS Team")
  (capabilities "sensor" "fs_write" "display")
  (memory_quota 81920)  ;; 80KB
  (description "Logs sensor data to file")
)
```

**Fallback JSON (Deprecated):**
```json
{
  "name": "sensor_logger",
  "version": "1.2.0",
  "capabilities": ["sensor", "fs_write", "display"],
  "memory_quota": 81920
}
```

**Manifest Parsing:**
1. Try to extract embedded `akira-manifest` custom section
2. If not found, look for `<app_name>.json` in same directory
3. If neither found, use default minimal capabilities

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
- HTTP server (port 80)
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

1. **No Secure Storage** - Apps can read each other's data files
2. **No App Signing** - No verification of app authenticity
3. **Coarse Capabilities** - All sensors lumped into `CAP_SENSOR_READ`
4. **No Network Isolation** - Apps share network stack
5. **No Process Isolation** - All apps run in same address space

## Future Enhancements

- **App Signing** - Code signing with public key verification
- **Secure Storage** - Per-app encrypted storage
- **Fine-Grained Caps** - Per-sensor, per-network capabilities
- **Secure Element Integration** - TPM/HSM for key storage
- **SELinux-style Policies** - Advanced MAC policies
- **Audit Logging** - Security event logging

## Security Checklist

**Before Deploying an App:**
- [ ] Review requested capabilities
- [ ] Test with quota limits enabled
- [ ] Check for excessive network usage
- [ ] Verify app behavior matches description
- [ ] Scan for known malicious patterns

**Before Firmware Update:**
- [ ] Verify signature (if signing enabled)
- [ ] Check version (prevent downgrades)
- [ ] Test in staging environment
- [ ] Backup current configuration
- [ ] Ensure rollback capability

## Related Documentation

- [Architecture Overview](index.md)
- [Runtime Architecture](runtime.md)
- [Manifest Format](../api-reference/manifest-format.md)
- [OTA Updates](../development/ota-updates.md)
