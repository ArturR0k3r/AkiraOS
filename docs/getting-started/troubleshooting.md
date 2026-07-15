# Troubleshooting Guide

Common issues and their solutions when working with AkiraOS.

## Build Issues

### "west: command not found"

**Cause:** West not installed or not in PATH

**Solution:**
```bash
pip3 install --user west
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
west --version  # Verify
```

---

### "CMake Error: Zephyr SDK not found"

**Cause:** SDK environment variables not set

**Solution:**
```bash
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4
export ZEPHYR_BASE=~/akira-workspace/zephyr

# Add to ~/.bashrc for persistence:
echo 'export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4' >> ~/.bashrc
echo 'export ZEPHYR_BASE=~/akira-workspace/zephyr' >> ~/.bashrc
```

---

### "hal_espressif not found" (ESP32 Platforms)

**Cause:** ESP-IDF HAL blobs not fetched

**Solution:**
```bash
cd ~/akira-workspace
west blobs fetch hal_espressif
```

---

### Build fails with "stack overflow" errors

**Cause:** Insufficient thread stack size

**Solution:**
Edit board config or `prj.conf`:
```bash
CONFIG_MAIN_STACK_SIZE=8192   # Increase from 4096
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=4096
```

---

## Flashing Issues

### "Permission denied" on /dev/ttyUSB0

**Cause:** User not in `dialout` group

**Solution:**
```bash
sudo usermod -a -G dialout $USER
# Log out and back in, or:
newgrp dialout

# Verify:
groups  # Should list 'dialout'
```

---

### "Failed to connect to ESP32"

**Cause:** Wrong serial port or hardware not ready

**Solution:**
```bash
# List available ports
ls /dev/tty*

# Try manual flash with port specified
west flash --esp-device /dev/ttyUSB0

# Reset ESP32 manually (press BOOT + RST, release RST, release BOOT)
```

---

### Flash verification fails

**Cause:** Corrupt upload or bad flash

**Solution:**
```bash
# Erase flash completely
west espressif erase-flash

# Rebuild and reflash
cd ~/akira-workspace/AkiraOS
./build.sh --full-clean
./build.sh -b esp32s3_devkitm_esp32s3_procpu -r all
```

---

## Runtime Issues

### OTA upload times out after 120 seconds

**Cause:** Large firmware + slow network (fixed in improved version)

**Workaround (current):**
```bash
# Reduce firmware size
CONFIG_SIZE_OPTIMIZATIONS=y
CONFIG_LOG_DEFAULT_LEVEL=3  # Reduce logging

# Or upload in smaller chunks (split firmware)
```

**Workaround (longer term):** A fix is planned — direct flash writes will remove the timeout.

---

### WASM app fails to load

**Cause:** Various issues

**Debug steps:**
```bash
# Check file exists
AkiraOS:~$ fs ls /apps

# Check file size
AkiraOS:~$ fs stat /apps/app.wasm

# Enable debug logging
AkiraOS:~$ log enable wasm 4

# Try loading again
AkiraOS:~$ wasm load /apps/app.wasm
```

**Common causes:**
- File not uploaded
- Corrupt WASM binary
- Insufficient memory
- Missing manifest

---

### "Out of memory" when loading WASM

**Cause:** PSRAM heap exhausted or quota exceeded

**Solution:**
```bash
# Increase PSRAM pool in prj.conf
CONFIG_HEAP_MEM_POOL_SIZE=131072  # 128KB — increase if WASM heap is exhausted

# Or increase per-app quota in manifest
{
  "memory_quota": 131072  # 128KB instead of 64KB
}

# Check current usage
AkiraOS:~$ kernel stacks
AkiraOS:~$ wasm status
```

---

### App crashes with "capability denied"

**Cause:** Missing capability in manifest

**Solution:**
```json
{
  "capabilities": [
    "display.write",
    "input.read",
    "sensor.read",   // Add this
    "rf.transceive"  // Or this
  ]
}
```

**Check logs for denied capability:**
```bash
AkiraOS:~$ log list
[00:00:05.123] <err> security: App 'myapp' denied CAP_SENSOR_READ
```

---

## Network Issues

### WiFi won't connect

**Cause:** Wrong credentials or WiFi not initialized

**Debug:**
```bash
# Check WiFi config
AkiraOS:~$ net iface

# Check credentials in board config
# boards/esp32s3_devkitm_esp32s3_procpu.conf
CONFIG_WIFI_SSID="YourNetwork"
CONFIG_WIFI_PSK="YourPassword"

# Manual connect (if available)
AkiraOS:~$ wifi connect -s YourNetwork -p YourPassword
```

---

### HTTP server not responding

**Cause:** WiFi not connected or firewall

**Debug:**
```bash
# Check IP address
AkiraOS:~$ net iface
# Look for "IPv4 address: 192.168.x.x"

# Ping from PC
ping 192.168.x.x

# Check server is running
AkiraOS:~$ kernel threads
# Look for "http_server" thread

# Enable HTTP logging
AkiraOS:~$ log enable http 4
```

---

### Can't upload files via HTTP

**Cause:** File too large or buffer overflow

**Debug:**
```bash
# Check file size
ls -lh myapp.wasm

# Current limit: ~1MB for HTTP uploads

# Check the app-install endpoint (raw body; /upload is firmware only)
curl -X POST --data-binary @myapp.wasm \
  "http://192.168.x.x/api/apps/install?name=myapp" -v
```

---

## Performance Issues

### WASM app runs slowly

**Cause:** Interpreter mode (AOT disabled)

**Solution:**
```bash
# Enable AOT compilation (if flash space available)
CONFIG_WAMR_AOT_SUPPORT=y

# Optimize WASM build
wasm-opt -O3 myapp.wasm -o myapp_opt.wasm

# Reduce logging overhead
CONFIG_LOG_DEFAULT_LEVEL=2  # WARNING only
```

---

### High memory usage

**Debug tools:**
```bash
# Check thread stacks
AkiraOS:~$ kernel stacks

# Check heap usage
AkiraOS:~$ kernel heap

# Check WASM app usage
AkiraOS:~$ wasm status

# Enable stack checking
CONFIG_THREAD_STACK_INFO=y
CONFIG_INIT_STACKS=y
```

---

## Serial Console Issues

### No output on serial console

**Cause:** Wrong baud rate or port

**Solution:**
```bash
# Use correct baud rate (115200 for ESP32)
west espmonitor

# Or manually:
picocom -b 115200 /dev/ttyUSB0

# Try different USB port
ls /dev/ttyUSB* /dev/ttyACM*
```

---

### Garbled output

**Cause:** Baud rate mismatch

**Solution:**
```bash
# ESP32 default is 115200
CONFIG_UART_CONSOLE_BAUDRATE=115200

# After changing, rebuild and reflash
```

---

## Debugging Crashes

### Enable core dumps

```bash
# prj.conf
CONFIG_DEBUG_COREDUMP=y
CONFIG_DEBUG_COREDUMP_BACKEND_LOGGING=y

# Rebuild
cd ~/akira-workspace/AkiraOS
./build.sh -b esp32s3_devkitm_esp32s3_procpu
```

### Enable assertions

```bash
CONFIG_ASSERT=y
CONFIG_ASSERT_LEVEL=2
```

### GDB Debugging (Native Sim)

```bash
# Build with debug symbols
west build -b native_sim -- -DCMAKE_BUILD_TYPE=Debug

# Run under GDB
gdb ../build/zephyr/zephyr.exe
(gdb) run
# When crashes:
(gdb) bt  # Backtrace
```

---

## Getting More Help

### Enable Verbose Logging

```bash
# prj.conf
CONFIG_LOG_DEFAULT_LEVEL=4  # DEBUG
CONFIG_LOG_MAX_LEVEL=4

# Per-module logging
CONFIG_AKIRA_LOG_LEVEL_DBG=y
CONFIG_WAMR_LOG_LEVEL_DBG=y
```

### Collect Diagnostics

```bash
AkiraOS:~$ kernel version
AkiraOS:~$ kernel uptime
AkiraOS:~$ kernel threads
AkiraOS:~$ kernel stacks
AkiraOS:~$ net iface
AkiraOS:~$ fs ls /
AkiraOS:~$ wasm status
```

### Report Issues

When opening a GitHub issue, include:
- Board/platform
- Build command used
- Zephyr/SDK versions
- Full error log
- Steps to reproduce

---

## Known Limitations (Current Version)

| Issue | Status | Workaround |
|-------|--------|------------|
| OTA socket timeout | Fix planned | Upload smaller firmware |
| Max 2 concurrent running apps | By design | Adequate for embedded use |
| Single HTTP connection | Fix planned | Queue uploads sequentially |
| No app signing | Future feature | Manual verification |
| File-based WASM loading | Improvement planned | Works but uses more peak memory |

Fixes for these are tracked in [GitHub Issues](https://github.com/ArturR0k3r/AkiraOS/issues).

---

## Quick Reference

**Build Commands:**
```bash
./build.sh -b <board>         # Build
./build.sh -b <board> -r all  # Build, flash, monitor
west build -t menuconfig      # Configuration
west flash                    # Flash only
west espmonitor        # Serial console
```

**Shell Commands:**
```bash
wasm load /apps/app.wasm      # Load WASM app
wasm start app_name           # Start app
wasm stop app_name            # Stop app
wasm status                   # List apps
fs ls /apps                   # List files
log enable <module> 4         # Debug logging
```

**Config Locations:**
- Global: `prj.conf`
- Board-specific: `boards/<board>.conf`
- Overlays: `boards/<board>.overlay`

---

## SD Card Issues

### "SD card not present" / `app scan sd` fails

**Cause:** Card not detected at SPI level — not a filesystem problem.

1. Ensure the card is **fully seated** in the slot.
2. Format as **FAT32** — exFAT is not supported.
3. Try a different card (some budget cards have poor SPI compatibility).

See [SD Card Setup](../hardware/sd-card.md) for full formatting instructions and advanced diagnostics.

### "Found 0 apps" on a card that has apps

- Files must be in the `apps/` folder on the card root, not a subfolder.
- Extension must be `.wasm` (lowercase).
- Safely eject the card on the PC before removing it to flush write cache.

---

## Still Stuck?

- Check [SDK Troubleshooting](../development/sdk-troubleshooting.md) for app-level issues
- Read [Best Practices](../development/best-practices.md) to avoid common pitfalls
- [SD Card Setup](../hardware/sd-card.md) — full hardware reference for the SD slot
- Check [GitHub Issues](https://github.com/ArturR0k3r/AkiraOS/issues)
- Ask in [Discussions](https://github.com/ArturR0k3r/AkiraOS/discussions)
- Review [Architecture Docs](../architecture)
