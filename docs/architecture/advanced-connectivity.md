# Advanced Connectivity Layer

> **Status:** Matter, Thread, and AkiraMesh are implemented but under active development, use with caution. They require additional west.yml modules and are not enabled in the default `prj.conf`. Standard WiFi and BLE connectivity is documented in [Connectivity Layer](connectivity.md).

Hardware-agnostic implementation of Matter, Thread, and AkiraMesh protocols for AkiraOS.

## Architecture Overview

The Advanced Connectivity Layer introduces a **Radio Abstraction Layer (RAL)** that decouples protocol stacks from physical radio hardware, enabling protocols to work across all supported platforms (ESP32-S3, nRF54L15, and future devices).

```
┌─────────────────────────────────────────────────────────────┐
│                    Protocol Layer                           │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐        │
│  │   Matter    │  │   Thread    │  │  AkiraMesh  │        │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘        │
│         │                 │                 │                │
│         └─────────────────┴─────────────────┘                │
└─────────────────────────────────────┬───────────────────────┘
                                      │
┌─────────────────────────────────────┴───────────────────────┐
│              Radio Abstraction Layer (RAL)                  │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐        │
│  │  WiFi Radio │  │  BLE Radio  │  │ 802.15.4 R. │        │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘        │
└─────────┴────────────────┴────────────────┴────────────────┘
          │                 │                 │
┌─────────┴────────────────┴────────────────┴────────────────┐
│                    Hardware Layer                           │
│   ESP32 WiFi      ESP32 BLE      nRF54L15 802.15.4        │
└─────────────────────────────────────────────────────────────┘
```

## Components

### 1. Radio Abstraction Layer (RAL)

**Location:** `src/connectivity/radio/`, `include/connectivity/radio_interface.h`

Provides unified interface to all radio hardware with:
- **Runtime capability discovery** - Query what each radio supports
- **Zero-copy operations** - Direct buffer management where possible
- **Thread-safe access** - Concurrent protocol usage of different radios
- **Event-driven model** - Callbacks for RX, TX complete, scan results

**Radio Backends:**
- `radio_wifi.c` - IEEE 802.11 WiFi (ESP32, nRF7002)
- `radio_ble.c` - Bluetooth Low Energy 5.x 
- `radio_802154.c` - IEEE 802.15.4 (nRF54L15, CC2520)

### 2. Matter Protocol Stack

**Location:** `src/connectivity/matter/`, `include/connectivity/matter_manager.h`

Matter (Connected Home over IP) protocol support with:
- **Multi-transport support** - WiFi, Thread, BLE commissioning
- **QR code / manual pairing code generation**
- **Device commissioning** - BLE and soft-AP flows
- **OTA updates** - Matter OTA requestor/provider
- **Cluster attributes** - Simplified API for WASM apps

**Configuration:**
```kconfig
CONFIG_AKIRA_MATTER=y
CONFIG_AKIRA_MATTER_VENDOR_ID=0xFFF1
CONFIG_AKIRA_MATTER_PRODUCT_ID=0x8000
CONFIG_AKIRA_MATTER_DEVICE_TYPE=0x0100  # On/Off Light
```

**Note:** Full implementation requires adding ConnectedHomeOverIP module to `west.yml`:
```yaml
- name: matter
  url: https://github.com/project-chip/connectedhomeip
  revision: v1.3.0.0
  import: true
```

### 3. Thread Protocol Stack

**Location:** `src/connectivity/thread/`, `include/connectivity/thread_manager.h`

OpenThread mesh networking with:
- **Network formation** - Leader election, router/end device roles
- **Commissioning** - Thread joiner/commissioner protocols  
- **Border router** - Gateway to WiFi/Ethernet (optional)
- **IPv6 addressing** - RLOC, EID, mesh-local, global addresses

**Configuration:**
```kconfig
CONFIG_AKIRA_THREAD=y
CONFIG_AKIRA_THREAD_FTD=y  # Full Thread Device (router capable)
CONFIG_AKIRA_THREAD_BORDER_ROUTER=n  # Enable for gateway functionality
```

**Note:** Requires OpenThread module in `west.yml`:
```yaml
- name: openthread
  url: https://github.com/openthread/openthread
  revision: thread-reference-20240710
  import: true
```

### 4. AkiraMesh Protocol

**Location:** `src/connectivity/mesh/`, `include/connectivity/akira_mesh.h`

Custom lightweight mesh networking with:
- **Multi-transport** - BLE Mesh, 802.15.4, ESP-NOW (ESP32)
- **Multi-hop routing** - AODV-based route discovery
- **WASM app distribution** - Chunked transfer across mesh
- **State synchronization** - Shared state between nodes

**Configuration:**
```kconfig
CONFIG_AKIRA_MESH=y
CONFIG_AKIRA_MESH_MAX_NODES=32
CONFIG_AKIRA_MESH_MAX_HOPS=5
CONFIG_AKIRA_MESH_TRANSPORT_BLE=y
CONFIG_AKIRA_MESH_TRANSPORT_802154=y
```

**Protocol Features:**
- **Node discovery** - Periodic beacon broadcasts
- **Automatic routing** - Route discovery/maintenance  
- **Mesh OTA** - Distribute WASM apps to all nodes
- **Low overhead** - Minimal header (21 bytes: version, msg_type, ttl, src_id[8], dest_id[8], seq_num)

## Shell Commands

### Radio Manager
```bash
radio info           # List available radios and capabilities
radio stats <type>   # Show radio statistics (wifi|ble|802154)
```

### Matter
```bash
matter info          # Show Matter status
matter commission    # Start commissioning (displays QR code)
matter reset         # Factory reset
```

### Thread
```bash
thread info          # Show Thread network status
thread start         # Start Thread networking
thread stop          # Stop Thread network
```

### AkiraMesh
```bash
mesh info            # Show mesh statistics
mesh nodes           # List discovered nodes
mesh start           # Start mesh networking
mesh stop            # Stop mesh
```

## Hardware Platform Support

| Platform | WiFi | BLE | 802.15.4 | Matter | Thread | AkiraMesh |
|----------|------|-----|----------|--------|--------|-----------|
| ESP32-S3 | Yes | Yes | No | Yes (WiFi transport) | No | Yes (BLE) |
| nRF54L15 | No | Yes | Yes | Yes (Thread transport) | Yes | Yes (BLE + 802.15.4) |
| native_sim | No | Simulated | No | No | No | Simulated |

## Usage Examples

### Initialize Radio Manager
```c
#include "connectivity/radio_interface.h"

// Radio manager initializes automatically at boot
// Query available radios:
radio_handle_t *wifi = radio_manager_get(RADIO_TYPE_WIFI);
if (wifi) {
    radio_stats_t stats;
    radio_get_stats(wifi, &stats);
    printk("WiFi RSSI: %d dBm\n", stats.rssi);
}
```

### Matter Device Setup
```c
#include "connectivity/matter_manager.h"

// Configure Matter device
matter_config_t config = {
    .vendor_id = 0xFFF1,
    .product_id = 0x8000,
    .device_type = MATTER_DEVICE_TYPE_LIGHT,
    .transport = MATTER_TRANSPORT_WIFI,  // or MATTER_TRANSPORT_THREAD
    .discriminator = 3840,
    .setup_pin_code = 20202021,
};
strcpy(config.device_name, "Akira Light");
strcpy(config.serial_number, "AK-001");

matter_manager_init(&config);

// Start commissioning
matter_start_commissioning(300);  // 5 minute window

// Get QR code for easy pairing
char qr[128];
matter_get_qr_code(qr, sizeof(qr));
printk("Scan this: %s\n", qr);
```

### Thread Network Formation
```c
#include "connectivity/thread_manager.h"

// Configure Thread network
thread_config_t config = {
    .panid = 0x1234,
    .channel = 15,
    .is_ftd = true,  // Full Thread Device
    .enable_border_router = false,
};
strcpy(config.network_name, "AkiraNet");

thread_manager_init(&config);
thread_start();

// Network will automatically form or join
```

### AkiraMesh Node
```c
#include "connectivity/akira_mesh.h"

// Configure mesh node
akira_mesh_config_t config = {
    .role = AKIRA_MESH_ROLE_NODE,
    .transport = AKIRA_MESH_TRANSPORT_BLE,
    .max_hops = 5,
    .enable_auto_routing = true,
};
memcpy(config.node_id, my_node_id, 8);
strcpy(config.node_name, "Akira-001");

akira_mesh_init(&config);
akira_mesh_start();

// Send data to another node
akira_mesh_send(dest_node_id, data, len);

// Broadcast to all nodes
akira_mesh_broadcast(announcement, size, 3);  // Max 3 hops
```

## Integration with Existing Systems

### OTA Updates
All three protocols integrate with the existing OTA manager through the generic transport interface:
- **Matter:** Matter OTA cluster routes to OTA manager via transport interface
- **Thread:** Thread multicast routes to OTA manager via transport interface
- **AkiraMesh:** Chunk-based transfer routes to OTA manager via `akira_mesh_distribute_app()`

> **Note:** Protocol-specific OTA transport adapters (`ota_matter.c`, `ota_thread.c`, `ota_mesh.c`) are planned for future implementation. Current OTA transports: BLE, HTTP, Cloud, USB.

### WASM Native API
Protocol integration with WASM applications is planned:
- Matter cluster attribute access (planned)
- Thread IPv6 socket operations (planned)
- AkiraMesh send/broadcast operations (planned)

> **Note:** WASM native API bindings for advanced connectivity protocols are under development. Currently available transports use existing network APIs.

### Transport Interface
Radio layer integrates with existing `transport_interface.c`:
```c
// Register RAL as transport source
transport_register_handler(TRANSPORT_DATA_FIRMWARE, 
                          radio_firmware_handler, NULL, 10);
```

## Performance Characteristics

### Radio Abstraction Layer
- **Overhead:** <100 bytes per radio handle
- **Latency:** <1ms function call overhead
- **Memory:** ~2KB total (all backends)

### Matter
- **Flash:** ~200KB (with CHIP SDK)
- **RAM:** ~50KB runtime
- **Commissioning:** 10-30 seconds typical

### Thread
- **Flash:** ~150KB (with OpenThread)
- **RAM:** ~30KB runtime  
- **Network formation:** 5-15 seconds

### AkiraMesh
- **Flash:** ~15KB
- **RAM:** ~4KB + (nodes × 80 bytes)
- **Discovery:** <5 seconds for 10 nodes
- **Hop latency:** ~20ms per hop (BLE), ~5ms (802.15.4)

## Troubleshooting

### Radio Not Available
```
ERROR: WiFi radio not available for Matter transport
```
**Solution:** Check that WiFi is enabled in `prj.conf`:
```
CONFIG_WIFI=y
CONFIG_WIFI_ESP32=y
```

### Matter Commissioning Fails
```
ERROR: Matter commissioning timeout
```
**Solution:** Ensure BLE is working and check discriminator/PIN:
```bash
matter info  # Verify state
bt info      # Check BLE status
```

### Thread Won't Form Network
```
ERROR: Thread network formation failed
```
**Solution:** Verify 802.15.4 radio and channel:
```bash
radio stats 802154
thread info
```

### Mesh Nodes Not Discovered
**Solution:** Check beacon transmission:
```bash
mesh info  # Verify messages_sent > 0
radio stats ble  # Check TX packets
```

## License

Copyright (c) 2026 PenEngineering S.R.L
SPDX-License-Identifier: Apache-2.0
