# Building Your First WASM App

Create, build, and deploy a "Hello World" WebAssembly application on AkiraOS.

## Prerequisites

- AkiraOS firmware flashed and running
- WASM toolchain installed
- Basic C programming knowledge

## Install WASM Toolchain

### Option 1: WASI SDK (Recommended)

```bash
cd ~
wget https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-21/wasi-sdk-21.0-linux.tar.gz
tar xvf wasi-sdk-21.0-linux.tar.gz
export WASI_SDK_PATH=~/wasi-sdk-21.0
```

### Option 2: Emscripten

```bash
cd ~
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

## Hello World App

### Step 1: Create Project

```bash
cd ~/akira-workspace/AkiraOS/wasm_sample
mkdir hello_world && cd hello_world
```

### Step 2: Write the Code

**hello_world.c:**
```c
#include <stdint.h>

// Import AkiraOS native functions
__attribute__((import_module("akira")))
__attribute__((import_name("log")))
extern void akira_log(const char *message, uint32_t len);

__attribute__((import_module("akira")))
__attribute__((import_name("display_clear")))
extern int akira_display_clear(uint32_t color);

__attribute__((import_module("akira")))
__attribute__((import_name("display_pixel")))
extern int akira_display_pixel(uint32_t x, uint32_t y, uint32_t color);

// WASM export: Application entry point
__attribute__((export_name("_start")))
void app_main() {
    const char *msg = "Hello from WASM!";
    akira_log(msg, 17);
    
    // Clear screen to black
    akira_display_clear(0x000000);
    
    // Draw a red pixel at (100, 50)
    akira_display_pixel(100, 50, 0xFF0000);
}
```

### Step 3: Create Manifest

**hello_world.json:**
```json
{
  "name": "hello_world",
  "version": "1.0.0",
  "author": "Your Name",
  "capabilities": ["display", "log"],
  "memory_quota": 65536,
  "description": "My first WASM app"
}
```

### Step 4: Build WASM Binary

**Using WASI SDK:**
```bash
$WASI_SDK_PATH/bin/clang \
  --target=wasm32-wasi \
  --sysroot=$WASI_SDK_PATH/share/wasi-sysroot \
  -O3 \
  -Wl,--no-entry \
  -Wl,--export=_start \
  -Wl,--allow-undefined \
  -o hello_world.wasm \
  hello_world.c
```

**Using Emscripten:**
```bash
emcc hello_world.c \
  -O3 \
  -s STANDALONE_WASM \
  -s EXPORTED_FUNCTIONS='["_start"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -o hello_world.wasm
```

**Verify the output:**
```bash
ls -lh hello_world.wasm
# Should be ~2-5KB

file hello_world.wasm
# Should say: WebAssembly (wasm) binary module
```

### Step 5: Optimize (Optional)

```bash
# Install wasm-opt
sudo apt install binaryen

# Optimize for size
wasm-opt -Oz hello_world.wasm -o hello_world_opt.wasm

# Compare sizes
ls -lh hello_world*.wasm
```

## Deploy to Hardware

### Method 1: HTTP Upload (Recommended)

```bash
# Find ESP32 IP address (check serial console or router)
export ESP32_IP=192.168.1.100

# Upload WASM app
curl -X POST \
  -F "file=@hello_world.wasm" \
  http://$ESP32_IP/upload

# Upload manifest
curl -X POST \
  -F "file=@hello_world.json" \
  http://$ESP32_IP/upload
```

### Method 2: File System Pre-load

```bash
# Copy to FS partition before flashing
cd ~/akira-workspace/AkiraOS
cp wasm_sample/hello_world/hello_world.wasm storage/apps/
./build.sh -b esp32s3_devkitm_esp32s3_procpu -r all
```

## Run Your App

### Via Shell Command

```bash
# Connect to serial console
west espressif monitor

# In the shell:
uart:~$ wasm load /apps/hello_world.wasm
uart:~$ wasm start hello_world
```

**Expected output:**
```
[00:00:10.523] <inf> wasm: Loading app: /apps/hello_world.wasm
[00:00:10.687] <inf> wasm: App loaded: hello_world
[00:00:10.690] <inf> wasm: Starting app: hello_world
[00:00:10.692] <inf> app: Hello from WASM!
[00:00:10.698] <inf> display: Clear screen: 0x000000
[00:00:10.702] <inf> display: Draw pixel: (100, 50) = 0xFF0000
[00:00:10.705] <inf> wasm: App hello_world exited
```

### Via Autostart

Edit `prj.conf`:
```bash
CONFIG_AKIRA_AUTOSTART_APP="/apps/hello_world.wasm"
```

Rebuild and app will start on boot.

## Next Steps

### Add Input Handling

```c
__attribute__((import_module("akira")))
__attribute__((import_name("input_read_buttons")))
extern uint32_t akira_input_read_buttons();

void app_main() {
    akira_log("Waiting for button press...", 26);
    
    while (1) {
        uint32_t buttons = akira_input_read_buttons();
        if (buttons & 0x01) {  // Button 0 pressed
            akira_log("Button pressed!", 14);
            break;
        }
    }
}
```

### Read Sensors

```c
__attribute__((import_module("akira")))
__attribute__((import_name("sensor_read")))
extern int akira_sensor_read(uint32_t sensor_id, float *data);

void app_main() {
    float temp = 0.0f;
    int ret = akira_sensor_read(0, &temp);  // Sensor 0 = temperature
    
    if (ret == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Temperature: %.2f C", temp);
        akira_log(msg, strlen(msg));
    }
}
```

### Persistent Storage

```c
__attribute__((import_module("akira")))
__attribute__((import_name("fs_write")))
extern int akira_fs_write(const char *path, const uint8_t *data, uint32_t len);

void app_main() {
    const char *data = "Hello, filesystem!";
    akira_fs_write("/data/hello/message.txt", data, strlen(data));
}
```

## Debugging

### Check App Status

```bash
uart:~$ wasm status
```

### View Logs

```bash
uart:~$ log list   # List log sources
uart:~$ log enable akira 4   # Set debug level
```

### Common Issues

**"Failed to load WASM": **Check file exists
```bash
uart:~$ fs ls /apps
```

**"Permission denied":** Missing capability
```json
{
  "capabilities": ["display", "log", "input"]  # Add required caps
}
```

**"Out of memory":** Increase quota
```json
{
  "memory_quota": 131072  # 128KB instead of 64KB
}
```

## Build Automation

**Makefile:**
```makefile
WASI_SDK = ~/wasi-sdk-21.0
CC = $(WASI_SDK)/bin/clang
CFLAGS = --target=wasm32-wasi --sysroot=$(WASI_SDK)/share/wasi-sysroot -O3
LDFLAGS = -Wl,--no-entry -Wl,--export=_start -Wl,--allow-undefined

hello_world.wasm: hello_world.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	wasm-opt -Oz $@ -o $@

clean:
	rm -f hello_world.wasm

deploy:
	curl -X POST -F "file=@hello_world.wasm" http://$(ESP32_IP)/upload

.PHONY: clean deploy
```

**Usage:**
```bash
make                    # Build
make deploy ESP32_IP=192.168.1.100  # Build and upload
```

## API Reference

See [Native API Documentation](../api-reference/native-api.md) for complete list of available functions.

## Related Documentation

- [API Reference](../api-reference/) - Complete native API
- [Manifest Format](../api-reference/manifest-format.md) - Capability specification
- [Development Guide](../development/building-apps.md) - Advanced topics
- [Troubleshooting](troubleshooting.md) - Common issues
