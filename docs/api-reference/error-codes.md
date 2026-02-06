# Error Codes Reference

Standard error codes returned by AkiraOS native APIs.

## Error Code Format

All errors are **negative integers** following POSIX conventions.

**Success:** `0`  
**Errors:** Negative values (`-1` to `-255`)

## Standard Error Codes

| Code | Name | Description | Common Causes |
|------|------|-------------|---------------|
| **0** | `OK` | Success | Operation completed successfully |
| **-1** | `EPERM` | Operation not permitted | Missing root privileges (not used in WASM) |
| **-2** | `ENOENT` | No such file or directory | File path doesn't exist |
| **-3** | `EIO` | I/O error | Hardware communication failure |
| **-5** | `EIO` | I/O error | Sensor not responding |
| **-12** | `ENOMEM` | Out of memory | Memory quota exceeded, heap exhausted |
| **-13** | `EACCES` | Permission denied | Missing capability |
| **-14** | `EFAULT` | Bad address | Invalid pointer (WASM safety usually prevents this) |
| **-22** | `EINVAL` | Invalid argument | Parameter out of range or invalid format |
| **-28** | `ENOSPC` | No space left on device | Flash/file system full |
| **-110** | `ETIMEDOUT` | Connection timed out | Network operation timeout |
| **-113** | `EHOSTUNREACH` | No route to host | Network unavailable |

---

## API-Specific Errors

### Display APIs

```c
int ret = akira_display_clear(0xFF0000);
```

| Return | Meaning |
|--------|---------|
| `0` | Screen cleared successfully |
| `-EACCES` | Missing `CAP_DISPLAY_WRITE` |
| `-EIO` | Display driver error |

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
| `0` | Sensor read successfully |
| `-ENOENT` | Sensor ID not available on this hardware |
| `-EACCES` | Missing `CAP_SENSOR_READ` |
| `-EIO` | I2C/SPI communication error |
| `-ETIMEDOUT` | Sensor not responding |

---

### RF/Network APIs

```c
int sent = akira_rf_send(data, len);
```

| Return | Meaning |
|--------|---------|
| `> 0` | Bytes successfully sent |
| `-EACCES` | Missing `CAP_RF_TRANSCEIVE` |
| `-EINVAL` | Length > 256 bytes |
| `-EHOSTUNREACH` | WiFi/BT disconnected |
| `-ENOMEM` | Network buffer pool exhausted |

---

### File System APIs

```c
int ret = akira_fs_write(path, data, len);
```

| Return | Meaning |
|--------|---------|
| `>= 0` | Bytes written |
| `-EACCES` | Missing `CAP_FS_WRITE` or path outside `/data/<app_name>/` |
| `-ENOENT` | Parent directory doesn't exist |
| `-ENOSPC` | Flash partition full |
| `-EINVAL` | Invalid path (e.g., `../` attempts) |
| `-EIO` | Flash write error |

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
int ret = akira_fs_write("/data/myapp/log.txt", data, len);

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
  "capabilities": ["display"]
}
```

---

## Debugging Tips

### Enable Detailed Error Logging

```bash
uart:~$ log enable akira 4
uart:~$ log enable wasm 4
```

### Check System Status

```bash
uart:~$ wasm status        # Check app state
uart:~$ kernel stacks      # Check memory usage
uart:~$ fs df              # Check disk space
uart:~$ net iface          # Check network status
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
// Define standard error codes
#define OK          0
#define EPERM      -1
#define ENOENT     -2
#define EIO        -3
#define ENOMEM    -12
#define EACCES    -13
#define EFAULT    -14
#define EINVAL    -22
#define ENOSPC    -28
#define ETIMEDOUT -110
#define EHOSTUNREACH -113

// Usage
int ret = akira_sensor_read(0, &temp);
if (ret == EACCES) {
    // Permission denied
}
```

---

## Related Documentation

- [Native API Reference](native-api.md) - Function return values
- [Manifest Format](manifest-format.md) - Capability configuration
- [Troubleshooting Guide](../getting-started/troubleshooting.md) - Common issues
- [Security Model](../architecture/security.md) - Permission system
