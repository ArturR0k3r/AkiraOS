# Native Simulation Platform

Run AkiraOS on your development PC for fast testing and debugging.

## Overview

Native simulation compiles AkiraOS to run directly on your host CPU (x86_64 or ARM64) without requiring physical hardware. This enables rapid development iteration and GDB debugging.

## Features

✅ **Fast Iteration** - No flashing, instant execution  
✅ **GDB Debugging** - Full source-level debugging  
✅ **CI/CD Friendly** - Automated testing  
✅ **Cross-Platform** - Linux, macOS (limited), WSL2  
⚠️ **Limited Peripherals** - No real WiFi/BT/sensors  

## Building

### Basic Build

```bash
cd ~/akira-workspace/AkiraOS
./build.sh -b native_sim
```

### Run

```bash
cd ../build/zephyr
./zephyr.exe
```

**Expected output:**
```
*** Booting Zephyr OS build v4.3.0 ***
[00:00:00.000] <inf> main: AkiraOS v1.4.7 starting...
[00:00:00.010] <inf> wasm: Runtime initialized
uart:~$ 
```

### Exit

Press `Ctrl+C` to stop execution.

---

## Simulated Features

| Feature | Status | Notes |
|---------|--------|-------|
| **WASM Runtime** | ✅ Full | Same as hardware |
| **File System** | ✅ Full | Uses host filesystem |
| **Shell** | ✅ Full | Interactive commands |
| **Display** | ✅ SDL2 | Graphical window (if SDL2 installed) |
| **Sensors** | ⚠️ Mock | Returns fake data |
| **WiFi** | ❌ | Not simulated |
| **Bluetooth** | ❌ | Not simulated |
| **OTA** | ❌ | Not applicable |

---

## Configuration

### SDL2 Display (Optional)

Install SDL2 for graphical display simulation:

```bash
sudo apt install libsdl2-dev
```

**Enable in config:**
```bash
CONFIG_DISPLAY=y
CONFIG_SDL_DISPLAY=y
CONFIG_SDL_DISPLAY_DEV_NAME="sdl_display"
```

Rebuild and run - a window will appear showing display output.

---

## File System

Native sim uses host filesystem at `/tmp/akiraos-fs/`:

```bash
# Files are stored on your PC
ls /tmp/akiraos-fs/
ls /tmp/akiraos-fs/apps/

# Copy WASM apps directly
cp wasm_sample/example_app.wasm /tmp/akiraos-fs/apps/

# In AkiraOS:
uart:~$ fs ls /apps
uart:~$ wasm load /apps/example_app.wasm
```

---

## Debugging

### GDB Debugging

```bash
# Build with debug symbols
west build -b native_sim -- -DCMAKE_BUILD_TYPE=Debug

# Run under GDB
gdb ../build/zephyr/zephyr.exe

(gdb) break main
(gdb) run
(gdb) next
(gdb) print variable_name
(gdb) backtrace
```

### Valgrind (Memory Debugging)

```bash
valgrind --leak-check=full ../build/zephyr/zephyr.exe
```

### Address Sanitizer

```bash
west build -b native_sim -- -DCMAKE_C_FLAGS="-fsanitize=address"
./build/zephyr/zephyr.exe
```

---

## Use Cases

### Algorithm Development

Test WASM app logic without hardware:

```c
// Develop and test locally
void process_data() {
    float data;
    akira_sensor_read(0, &data);  // Returns mock data
    // ... test algorithm ...
}
```

### Shell Command Testing

Test shell commands:

```bash
uart:~$ wasm status
uart:~$ fs ls /
uart:~$ kernel threads
uart:~$ log list
```

### CI/CD Integration

```yaml
# .github/workflows/test.yml
- name: Build Native Sim
  run: |
    cd AkiraOS
    ./build.sh -b native_sim
    
- name: Run Tests
  run: |
    timeout 10s ../build/zephyr/zephyr.exe || true
```

---

## Limitations

❌ **No Real Hardware**
- WiFi/BT not available
- Sensors return fake values
- GPIO/I2C/SPI not functional

❌ **Different Timing**
- Host CPU much faster than embedded
- Sleep/delays not accurately simulated
- Real-time behavior different

❌ **Different Memory Layout**
- No PSRAM distinction
- Larger address space
- Different alignment rules

**Recommendation:** Use native_sim for logic testing, validate on real hardware before deployment.

---

## Mock Data

Simulated sensor values (can be customized):

```c
// In sensor HAL stub
float mock_sensor_read(uint32_t id) {
    switch (id) {
        case 0: return 25.0f;  // Temperature: 25°C
        case 1: return 60.0f;  // Humidity: 60%
        case 2: return 1013.25f;  // Pressure: 1013.25 hPa
        default: return 0.0f;
    }
}
```

---

## Performance

Native sim runs much faster than embedded hardware:

| Operation | ESP32-S3 | Native Sim |
|-----------|----------|------------|
| Boot time | ~500ms | ~50ms |
| WASM load (100KB) | ~80ms | ~5ms |
| Shell responsiveness | Good | Excellent |

---

## Related Documentation

- [Platform Overview](index.md) - All platforms
- [Installation Guide](../getting-started/installation.md) - Setup
- [Development Guide](../development/) - Advanced usage
