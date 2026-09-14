---
layout: default
title: Error Codes
parent: API Reference
nav_order: 3
---

# Error Codes Reference

Standard error codes returned by AkiraOS native APIs.

## Error Code Format

AkiraOS uses three error value ranges:

| Range | Type | Description |
|-------|------|-------------|
| `0` | Success | Operation completed successfully |
| Negative | System error | Standard POSIX `errno` values (e.g. `-EINVAL`, `-ENOMEM`) |
| `>= 1000` | Domain error | AkiraOS-specific `AKIRA_ERR_*` positive codes |

## Standard Error Codes

| Code | Name | Description | Common Causes |
|------|------|-------------|---------------|
| **0** | `OK` | Success | Operation completed successfully |
| **-1** | `EPERM` | Operation not permitted | Capability check failed (storage/RF APIs) |
| **-2** | `ENOENT` | No such file or directory | File path doesn't exist |
| **-5** | `EIO` | I/O error | Hardware communication failure, sensor not responding |
| **-12** | `ENOMEM` | Out of memory | Memory quota exceeded, heap exhausted |
| **-13** | `EACCES` | Permission denied | Missing capability |
| **-14** | `EFAULT` | Bad address | Invalid pointer (WASM safety usually prevents this) |
| **-22** | `EINVAL` | Invalid argument | Parameter out of range or invalid format |
| **-28** | `ENOSPC` | No space left on device | Flash/file system full |
| **-110** | `ETIMEDOUT` | Connection timed out | Network operation timeout |
| **-113** | `EHOSTUNREACH` | No route to host | Network unavailable |

---

## Core Error Macros (`akira.h`)

Shorthand macros used by internal OS code:

| Code | Name | Description |
|------|------|-------------|
| `0` | `AKIRA_OK` | Success |
| `-1` | `AKIRA_ERR_INVALID` | Invalid argument |
| `-2` | `AKIRA_ERR_NOMEM` | Out of memory |
| `-3` | `AKIRA_ERR_EXISTS` | Already exists |
| `-4` | `AKIRA_ERR_NOT_FOUND` | Not found |
| `-5` | `AKIRA_ERR_BUSY` | Resource busy |
| `-6` | `AKIRA_ERR_TIMEOUT` | Operation timed out |
| `-7` | `AKIRA_ERR_PERMISSION` | Permission denied |
| `-8` | `AKIRA_ERR_NOT_READY` | Not initialized/ready |
| `-9` | `AKIRA_ERR_IO` | I/O error |
| `-10` | `AKIRA_ERR_UNSUPPORTED` | Not supported |

> **Note:** These do not map to POSIX `errno` values. Use POSIX `errno` constants when interfacing with Zephyr APIs or returning errors to WASM.

---

## Domain Error Codes (`error_codes.h`)

Positive codes (`>= 1000`) for structured error handling across subsystems:

### Error Handling Convention

```c
int ret = app_manager_install(...);
if (ret < 0) {
    LOG_ERR("System error: %d", ret);      // POSIX errno
} else if (ret > 0) {
    LOG_ERR("%s", akira_strerror(ret));     // Domain error
}
```

### App Manager (1001–1007)

| Code | Name | Description |
|------|------|-------------|
| `1001` | `AKIRA_ERR_APP_NOT_FOUND` | Application not installed |
| `1002` | `AKIRA_ERR_APP_ALREADY_EXISTS` | App already installed |
| `1003` | `AKIRA_ERR_APP_RUNNING` | App is currently running |
| `1004` | `AKIRA_ERR_APP_NOT_RUNNING` | App is not running |
| `1005` | `AKIRA_ERR_APP_FAILED` | App execution failed |
| `1006` | `AKIRA_ERR_APP_MAX_REACHED` | Max installed apps reached |
| `1007` | `AKIRA_ERR_APP_MAX_RUNNING` | Max concurrent apps reached |

### WASM Runtime (1100–1105)

| Code | Name | Description |
|------|------|-------------|
| `1100` | `AKIRA_ERR_WASM_INVALID` | Invalid WASM binary |
| `1101` | `AKIRA_ERR_WASM_TOO_LARGE` | Binary exceeds size limit |
| `1102` | `AKIRA_ERR_WASM_LOAD_FAILED` | Failed to load module |
| `1103` | `AKIRA_ERR_WASM_EXEC_FAILED` | Execution error |
| `1104` | `AKIRA_ERR_WASM_OUT_OF_MEMORY` | WASM heap exhausted |
| `1105` | `AKIRA_ERR_WASM_INSTANTIATE` | Module instantiation failed |

### Storage (1200–1204)

| Code | Name | Description |
|------|------|-------------|
| `1200` | `AKIRA_ERR_STORAGE_FULL` | Partition full |
| `1201` | `AKIRA_ERR_STORAGE_QUOTA` | App storage quota exceeded |
| `1202` | `AKIRA_ERR_STORAGE_CORRUPTED` | Filesystem corruption detected |
| `1203` | `AKIRA_ERR_PATH_INVALID` | Malformed path |
| `1204` | `AKIRA_ERR_PATH_TRAVERSAL` | Path traversal attempt (`../`) |

### Network (1300–1313)

| Code | Name | Description |
|------|------|-------------|
| `1300` | `AKIRA_ERR_NET_NOT_CONNECTED` | No network connection |
| `1301` | `AKIRA_ERR_NET_TIMEOUT` | Network operation timed out |
| `1302` | `AKIRA_ERR_NET_DNS_FAILED` | DNS resolution failed |
| `1303` | `AKIRA_ERR_NET_TLS_FAILED` | TLS handshake failed |
| `1310` | `AKIRA_ERR_HTTP_BAD_REQUEST` | HTTP 400 |
| `1311` | `AKIRA_ERR_HTTP_UNAUTHORIZED` | HTTP 401 |
| `1312` | `AKIRA_ERR_HTTP_NOT_FOUND` | HTTP 404 |
| `1313` | `AKIRA_ERR_HTTP_SERVER_ERROR` | HTTP 5xx |

### Cloud (1400–1403)

| Code | Name | Description |
|------|------|-------------|
| `1400` | `AKIRA_ERR_CLOUD_NOT_CONNECTED` | Cloud not connected |
| `1401` | `AKIRA_ERR_CLOUD_AUTH_FAILED` | Authentication failed |
| `1402` | `AKIRA_ERR_CLOUD_PROTOCOL` | Protocol error |
| `1403` | `AKIRA_ERR_CLOUD_RATE_LIMIT` | Rate limit exceeded |

### OTA (1500–1503)

| Code | Name | Description |
|------|------|-------------|
| `1500` | `AKIRA_ERR_OTA_IN_PROGRESS` | OTA update already running |
| `1501` | `AKIRA_ERR_OTA_INVALID_IMAGE` | Bad firmware image |
| `1502` | `AKIRA_ERR_OTA_VERIFY_FAILED` | Image verification failed |
| `1503` | `AKIRA_ERR_OTA_NO_SPACE` | Insufficient space for update |

### Security (1600–1603)

| Code | Name | Description |
|------|------|-------------|
| `1600` | `AKIRA_ERR_PERMISSION_DENIED` | Permission denied |
| `1601` | `AKIRA_ERR_CAPABILITY_MISSING` | Required capability not in manifest |
| `1602` | `AKIRA_ERR_SIGNATURE_INVALID` | Code signature invalid |
| `1603` | `AKIRA_ERR_CERTIFICATE_INVALID` | Certificate invalid or expired |

### Bluetooth (1700–1704)

| Code | Name | Description |
|------|------|-------------|
| `1700` | `AKIRA_ERR_BLE_NOT_ENABLED` | Bluetooth not enabled |
| `1701` | `AKIRA_ERR_BLE_SCAN_FAILED` | Scan failed to start |
| `1702` | `AKIRA_ERR_BLE_CONNECT_FAILED` | Connection failed |
| `1703` | `AKIRA_ERR_BLE_DISCONNECTED` | Unexpectedly disconnected |
| `1704` | `AKIRA_ERR_BLE_GATT_FAILED` | GATT operation failed |

### Sensor (1800–1803)

| Code | Name | Description |
|------|------|-------------|
| `1800` | `AKIRA_ERR_SENSOR_NOT_FOUND` | Sensor not available |
| `1801` | `AKIRA_ERR_SENSOR_NOT_READY` | Sensor not initialized |
| `1802` | `AKIRA_ERR_SENSOR_READ_FAILED` | Read operation failed |
| `1803` | `AKIRA_ERR_SENSOR_CALIBRATION` | Calibration error |

### Display (1900–1902)

| Code | Name | Description |
|------|------|-------------|
| `1900` | `AKIRA_ERR_DISPLAY_NOT_READY` | Display not initialized |
| `1901` | `AKIRA_ERR_DISPLAY_BUSY` | Display busy |
| `1902` | `AKIRA_ERR_DISPLAY_BAD_PARAMS` | Invalid display parameters |

### Generic (2000–2005)

| Code | Name | Description |
|------|------|-------------|
| `2000` | `AKIRA_ERR_NOT_INITIALIZED` | Subsystem not initialized |
| `2001` | `AKIRA_ERR_ALREADY_INITIALIZED` | Already initialized |
| `2002` | `AKIRA_ERR_NOT_SUPPORTED` | Feature not supported on this platform |
| `2003` | `AKIRA_ERR_INTERNAL` | Internal error |
| `2004` | `AKIRA_ERR_TIMEOUT` | Generic timeout |
| `2005` | `AKIRA_ERR_WOULD_BLOCK` | Operation would block |

---

## Error Helper Functions

Defined in `error_codes.h`:

```c
// Convert domain error code to readable string
const char *akira_strerror(int error);

// Check error category
bool akira_is_system_error(int error);  // error < 0
bool akira_is_domain_error(int error);  // error >= 1000
bool akira_is_success(int error);       // error == 0
```

---

## API-Specific Errors

### Display APIs

```c
// WASM export — returns int
int ret = akira_display_clear(0xFF0000);
```

| Return | Meaning |
|--------|---------|
| `0` | Screen cleared successfully |
| `-EACCES` | Missing `CAP_DISPLAY_WRITE` |

---

### Input APIs

```c
uint32_t buttons = akira_input_read_buttons();
```

**Note:** Input APIs return data directly, errors indicated by special values:
- Touch functions: `-1` = no touch
- Button functions: Always succeed (returns bitmask)

---

### Sensor APIs

```c
float temp;
int ret = akira_sensor_read(0, &temp);
```

| Return | Meaning |
|--------|---------|
| `0` | Sensor read successfully (`akira_sensor_read`) |
| `INT32_MIN` | Any error from WASM export |
| `-ENOTSUP` | No device supports the requested channel |
| `-EACCES` | Missing `CAP_SENSOR_READ` (WASM export only) |
| `-EINVAL` | NULL output pointer |

---

### RF/Network APIs

```c
int sent = akira_rf_send(data, len);
```

| Return | Meaning |
|--------|---------|
| `> 0` | Bytes successfully sent |
| `-EPERM` | Missing `CAP_RF_TRANSCEIVE` |
| `-EINVAL` | Zero-length payload |
| `-ENOSYS` | RF framework not compiled in (`CONFIG_AKIRA_RF_FRAMEWORK` not set) |

---

### Storage APIs

```c
int fd = storage_open("log.txt", STORAGE_O_WRITE);
int ret = storage_write(fd, data, len);
storage_close(fd);
```

| Return | Meaning |
|--------|---------|
| `>= 0` | Bytes written / file descriptor |
| `-EPERM` | Missing `storage.read` or `storage.write` capability |
| `-EACCES` | Path traversal attempt (`../`) |
| `-EINVAL` | Empty path or invalid arguments |
| `-EMFILE` | Too many open file descriptors |
| `-EBADF` | Invalid or foreign file descriptor |
| `-ENOENT` | Parent directory doesn't exist |

---

### Memory APIs

```c
void *ptr = malloc(size);
```

| Return | Meaning |
|--------|---------|
| `!= NULL` | Allocation successful |
| `NULL` | Out of memory (quota exceeded or heap exhausted) |

---

## Error Handling Patterns

### Check Return Values

```c
int ret = akira_sensor_read(0, &temp);
if (ret != 0) {
    akira_log("Sensor error", 12);
    // Handle error
}
```

### Specific Error Handling

```c
int fd = storage_open("log.txt", STORAGE_O_WRITE);
if (fd < 0) {
    akira_log("Open failed", 11);
    return;
}
int ret = storage_write(fd, data, len);
storage_close(fd);

if (ret >= 0) {
    akira_log("Write successful", 16);
} else if (ret == -EACCES) {
    akira_log("Permission denied", 17);
} else if (ret == -ENOSPC) {
    akira_log("Disk full", 9);
} else {
    akira_log("Unknown error", 13);
}
```

### Retry Logic

```c
int tries = 0;
int ret;

do {
    ret = akira_rf_send(data, len);
    if (ret > 0) break;
    
    akira_sleep_ms(100);  // Wait before retry
    tries++;
} while (tries < 3);

if (ret < 0) {
    akira_log("Send failed after retries", 25);
}
```

---

## Logging Errors

Use error codes in log messages:

```c
int ret = akira_sensor_read(0, &temp);
if (ret != 0) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Sensor error: %d", ret);
    akira_log(msg, strlen(msg));
}
```

**Example output:**
```
[00:00:05.123] <inf> app: Sensor error: -2
```

---

## Quota Exceeded Handling

When memory quota is exceeded:

```c
void *buffer = malloc(large_size);
if (buffer == NULL) {
    akira_log("Out of memory", 13);
    
    // Options:
    // 1. Free existing allocations
    // 2. Reduce allocation size
    // 3. Fail gracefully
    
    return -ENOMEM;
}
```

**Prevention:**
- Monitor quota usage
- Free memory promptly
- Use stack variables when possible
- Request appropriate quota in manifest

---

## Permission Denied Handling

When capability is missing:

```c
int ret = akira_display_clear(0x000000);
if (ret == -EACCES) {
    akira_log("Display access denied", 21);
    akira_log("Add 'display' to manifest", 25);
    
    // App should exit or work in degraded mode
}
```

**Fix:** Update manifest:
```json
{
  "capabilities": ["display.write"]
}
```

---

## Debugging Tips

### Enable Detailed Error Logging

```bash
AkiraOS:~$ log enable akira 4
AkiraOS:~$ log enable wasm 4
```

### Check System Status

```bash
AkiraOS:~$ wasm status        # Check app state
AkiraOS:~$ kernel stacks      # Check memory usage
AkiraOS:~$ fs df              # Check disk space
AkiraOS:~$ net iface          # Check network status
```

### Common Error Scenarios

| Error | Check | Solution |
|-------|-------|----------|
| `-EACCES` | Manifest capabilities | Add required capability |
| `-ENOMEM` | `wasm status`, `kernel heap` | Increase `memory_quota` |
| `-ENOSPC` | `fs df` | Delete old files |
| `-EIO` | Hardware logs | Check connections |
| `-ENOENT` | `fs ls` | Verify file path |

---

## Error Code Constants (C)

For use in WASM apps (not automatically defined):

```c
// errno.h is available — use standard constants directly.
// These values are provided for reference only; do NOT redefine them.
// EPERM        1   (returned as -EPERM  = -1)
// ENOENT       2   (returned as -ENOENT = -2)
// EIO          5   (returned as -EIO   = -5)
// ENOMEM      12   (returned as -ENOMEM = -12)
// EACCES      13   (returned as -EACCES = -13)
// EFAULT      14   (returned as -EFAULT = -14)
// EINVAL      22   (returned as -EINVAL = -22)
// ENOTSUP     95   (returned as -ENOTSUP = -95)
// ETIMEDOUT  110   (returned as -ETIMEDOUT = -110)
// EHOSTUNREACH 113 (returned as -EHOSTUNREACH = -113)

// Usage
#include <errno.h>
int ret = akira_sensor_read(0, &temp);
if (ret == -EPERM) {
    // Capability check failed
}
```

---

## Related Documentation

- [Native API Reference](native-api.md) - Function return values
- [Manifest Format](manifest-format.md) - Capability configuration
- [Troubleshooting Guide](../getting-started/troubleshooting.md) - Common issues
- [Security Model](../architecture/security.md) - Permission system

---

*Last updated: 2026-09-14 (AkiraOS v1.6.4)*
