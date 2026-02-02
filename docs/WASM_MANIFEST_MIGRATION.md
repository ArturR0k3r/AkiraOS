# WASM Application Manifest Migration Guide

This guide helps developers migrate from the legacy JSON-based manifest format to the new embedded WASM manifest format in AkiraOS v1.2+.

## Overview

AkiraOS has transitioned from external JSON manifest files to **embedded custom sections** within WASM binaries. This approach:

- Eliminates the need for separate manifest files
- Ensures manifest-binary integrity (can't have mismatched manifest/binary)
- Reduces storage overhead (~200-500 bytes saved per app)
- Simplifies deployment (single file distribution)

## Migration Steps

### 1. Remove External JSON Manifest

**Before (Legacy):**
```
my_app/
├── manifest.json      # Remove this
└── app.wasm
```

**After (New):**
```
my_app/
└── app.wasm           # Manifest embedded inside
```

### 2. Update Your Build Process

Add the manifest as a custom section during WASM compilation.

#### Using wasm-ld (LLVM)

```bash
# Create manifest binary
echo -n '{"name":"my_app","version":"1.0.0","capabilities":["display","time"]}' > manifest.bin

# Link with custom section
wasm-ld -o app.wasm app.o \
    --export=app_main \
    --export=app_init \
    --custom-section=.akira.manifest=manifest.bin
```

#### Using wasm-tools

```bash
# Add section to existing WASM
wasm-tools custom-section add \
    --section-name ".akira.manifest" \
    --section-data manifest.json \
    app.wasm -o app_with_manifest.wasm
```

### 3. Manifest Format Reference

The embedded manifest uses the same JSON structure:

```json
{
  "name": "my_app",
  "version": "1.0.0",
  "description": "My AkiraOS Application",
  "author": "Your Name",
  "capabilities": ["display", "time", "sensors", "network", "storage"],
  "memory": {
    "heap_size": 65536,
    "stack_size": 8192
  },
  "entry_point": "app_main"
}
```

#### Required Fields

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | App identifier (max 31 chars, alphanumeric + underscore) |
| `version` | string | Semantic version (e.g., "1.2.3") |
| `capabilities` | array | List of required capabilities |

#### Optional Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `description` | string | "" | Human-readable description |
| `author` | string | "" | Application author |
| `memory.heap_size` | number | 65536 | WASM heap size in bytes |
| `memory.stack_size` | number | 8192 | WASM stack size in bytes |
| `entry_point` | string | "app_main" | Main function name |

### 4. Available Capabilities

| Capability | Description |
|------------|-------------|
| `display` | Access display/UI APIs |
| `time` | Access system time/timers |
| `sensors` | Read sensor data |
| `network` | Network/HTTP access |
| `storage` | File system access |
| `rf` | RF/radio access |
| `admin` | Administrative functions |

### 5. Example: Complete Migration

**Old manifest.json:**
```json
{
  "app_name": "sensor_logger",
  "app_version": "2.1.0",
  "required_permissions": ["sensors", "storage", "time"],
  "heap_kb": 64,
  "stack_kb": 8
}
```

**New embedded manifest:**
```json
{
  "name": "sensor_logger",
  "version": "2.1.0",
  "capabilities": ["sensors", "storage", "time"],
  "memory": {
    "heap_size": 65536,
    "stack_size": 8192
  }
}
```

**Build script update:**
```bash
#!/bin/bash
# build_wasm_app.sh

APP_NAME="sensor_logger"

# Create manifest
cat > manifest.json << 'EOF'
{"name":"sensor_logger","version":"2.1.0","capabilities":["sensors","storage","time"],"memory":{"heap_size":65536,"stack_size":8192}}
EOF

# Compile to object
clang --target=wasm32 -nostdlib -O2 -c ${APP_NAME}.c -o ${APP_NAME}.o

# Link with embedded manifest
wasm-ld -o ${APP_NAME}.wasm ${APP_NAME}.o \
    --no-entry \
    --export=app_main \
    --export=app_init \
    --allow-undefined \
    --custom-section=.akira.manifest=manifest.json

echo "Built ${APP_NAME}.wasm with embedded manifest"
```

### 6. Validation

Use the AkiraOS shell to verify your app's manifest:

```
akira:~$ app info sensor_logger
Name: sensor_logger
Version: 2.1.0
Capabilities: sensors, storage, time
Heap: 64 KB
Stack: 8 KB
State: installed
```

### 7. Backward Compatibility

AkiraOS v1.2+ will:

1. First check for embedded `.akira.manifest` section
2. Fall back to external `manifest.json` if not found (deprecated)
3. Use defaults if neither exists (limited capabilities)

**Note:** External manifest support will be removed in v2.0. Migrate before then.

## Troubleshooting

### "Invalid manifest" error

- Ensure JSON is valid (use a JSON validator)
- Check for null bytes or BOM at start of manifest
- Verify section name is exactly `.akira.manifest`

### "Capability denied" after migration

- Verify all required capabilities are listed in manifest
- Capabilities are case-sensitive: use lowercase

### App loads but crashes

- Check `entry_point` matches your main function export
- Verify memory settings are sufficient for your app
- Ensure all exported functions use correct signatures

## API Reference

For programmatic manifest parsing, see [manifest_parser.h](../include/manifest_parser.h):

```c
#include "manifest_parser.h"

// Parse manifest from WASM binary
int manifest_parse_from_wasm(const uint8_t *wasm_data, size_t wasm_size,
                             struct app_manifest *manifest);

// Validate manifest structure
bool manifest_validate(const struct app_manifest *manifest);
```

---

*Migration guide for AkiraOS v1.2.3 | Last updated: 2025*
