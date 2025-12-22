# Pending Features 

This document tracks features that are planned but not yet fully implemented in AkiraOS.

---

## 🔴 High Priority

### 1. Inter-Process Communication (IPC) System

**Status:** Stub implementation exists, needs complete rewrite  
**Priority:** HIGH  
**Estimated Effort:** 2-3 weeks  

**Description:**  
Full IPC system for WASM app-to-app and app-to-system communication.

**Requirements:**
- [ ] Message bus with topic-based pub/sub
  - Wildcard topic matching (`sensors/*`, `system/+/status`)
  - Request-reply pattern with timeout
  - Message priority and ordering guarantees
  - Per-app message queue limits
  
- [ ] Shared memory regions
  - App-to-app zero-copy data transfer
  - Permission-based access control
  - Memory mapping into WASM linear memory
  - Reference counting and cleanup
  
- [ ] Security considerations
  - Caller app ID verification from WASM context
  - Capability-based access control
  - Rate limiting to prevent DoS
  - Message size limits

**Files:**
- `src/ipc/message_bus.c` - Currently stub with TODOs
- `src/ipc/shared_memory.c` - Currently stub with TODOs

**Design Notes:**
- Consider Zephyr's existing IPC mechanisms (pipes, message queues)
- Must work within WASM sandbox constraints
- Performance critical for sensor data streaming

---

### 2. Capability-Based Security System

**Status:** Not implemented (all checks commented out)  
**Priority:** HIGH  
**Estimated Effort:** 1-2 weeks  

**Description:**  
Fine-grained permission system for WASM apps.

**Requirements:**
- [ ] Define capability types
  - CAP_DISPLAY_WRITE, CAP_DISPLAY_READ
  - CAP_STORAGE_READ, CAP_STORAGE_WRITE
  - CAP_NETWORK_HTTP, CAP_NETWORK_BLE
  - CAP_SENSOR_READ, CAP_GPIO_WRITE
  - CAP_SYSTEM_REBOOT, CAP_OTA_UPDATE
  
- [ ] Manifest-based capability declaration
  ```json
  {
    "capabilities": [
      "display.write",
      "storage.read",
      "network.http"
    ]
  }
  ```
  
- [ ] Runtime enforcement in all APIs
  - Add checks before privileged operations
  - Return -EPERM on capability violation
  - Log security violations
  
- [ ] User consent for sensitive operations
  - Network access
  - Location/sensor data
  - Storage access

**Files Requiring Updates:**
- `src/api/akira_display_api.c` - All functions (12 TODOs)
- `src/api/akira_storage_api.c` - All functions (8 TODOs)
- `src/api/akira_gui_api.c` - Widget creation functions
- `src/api/akira_network_api.c` - HTTP/socket operations
- `src/api/akira_sensor_api.c` - Sensor read operations
- `src/api/akira_system_api.c` - System control functions

**Implementation Steps:**
1. Define capability bitflags in header
2. Add capability field to app_manifest_t
3. Parse capabilities from manifest JSON
4. Create capability_check() helper function
5. Add checks to all API entry points
6. Add shell command to view app capabilities

---

### 3. WASM Context API Enhancement

**Status:** Partial (cannot get caller app ID)  
**Priority:** HIGH  
**Estimated Effort:** 1 week  

**Description:**  
Ability to identify which WASM app is making a call.

**Requirements:**
- [ ] Get caller app ID from WASM runtime context
- [ ] Thread-local storage for current app ID
- [ ] Integration with OCRE/WAMR context APIs
- [ ] Helper function: `int wasm_get_current_app_id(void)`

**Use Cases:**
- Capability checking (which app is calling?)
- IPC message routing (sender identification)
- Storage path isolation (app-specific folders)
- Resource quota enforcement (per-app limits)

**Blocked By:**
- OCRE library API research needed
- May require WAMR native function enhancement

---

## 🟡 Medium Priority

### 4. Persistent Settings Storage

**Status:** Not implemented  
**Priority:** MEDIUM  
**Estimated Effort:** 3-5 days  

**Description:**  
Key-value settings storage with NVS backend.

**Requirements:**
- [ ] System settings (WiFi credentials, timezone, etc.)
- [ ] Per-app settings with namespace isolation
- [ ] Settings encryption for sensitive data
- [ ] Shell commands for settings management
- [ ] Settings sync to cloud (optional)

**API Design:**
```c
int akira_settings_get(const char *key, char *value, size_t max_len);
int akira_settings_set(const char *key, const char *value);
int akira_settings_delete(const char *key);
int akira_settings_list(settings_iterator_t *iter);
```

---

### 5. Advanced Scheduler Features

**Status:** Basic round-robin implemented  
**Priority:** MEDIUM  
**Estimated Effort:** 1 week  

**Description:**  
Enhanced scheduling algorithms and priority management.

**Planned Features:**
- [ ] Priority-based scheduling
- [ ] Real-time task support (hard deadlines)
- [ ] CPU quota enforcement (prevent CPU hogging)
- [ ] Cooperative scheduling (yield points)
- [ ] Background task scheduling
- [ ] Power-aware scheduling (sleep when idle)

**File:** `src/resource/scheduler.c`

---

### 6. Cloud Application Store

**Status:** Protocol designed, not implemented  
**Priority:** MEDIUM  
**Estimated Effort:** 2 weeks  

**Description:**  
Over-the-air app discovery, download, and installation.

**Requirements:**
- [ ] App catalog browsing
- [ ] Search and filtering
- [ ] App metadata (description, screenshots, ratings)
- [ ] Signature verification
- [ ] Automatic updates
- [ ] User reviews and ratings

---

### 7. USB Mass Storage Mode

**Status:** Not implemented  
**Priority:** MEDIUM  
**Estimated Effort:** 1 week  

**Description:**  
Expose filesystem via USB for easy file transfer.

**Requirements:**
- [ ] USB MSC device class implementation
- [ ] Read-only or read-write mode selection
- [ ] Eject handling
- [ ] Prevent filesystem corruption during access

---

## 🟢 Low Priority / Future Ideas

### 8. Power Management

**Status:** Basic power manager exists  
**Priority:** LOW  
**Estimated Effort:** 1-2 weeks  

**Features:**
- [ ] Sleep modes (light sleep, deep sleep)
- [ ] Wake sources (button, timer, BLE)
- [ ] Per-peripheral power control
- [ ] Battery level monitoring
- [ ] Power budget enforcement

---

### 9. Audio Support

**Status:** Not implemented  
**Priority:** LOW  
**Estimated Effort:** 2-3 weeks  

**Features:**
- [ ] Audio playback API
- [ ] Audio recording API
- [ ] Codec support (MP3, AAC, FLAC)
- [ ] Mixer for multiple audio sources
- [ ] Volume control

---

### 10. Camera Support

**Status:** Not implemented  
**Priority:** LOW  
**Estimated Effort:** 2-3 weeks  

**Features:**
- [ ] Camera frame capture
- [ ] Image format conversion
- [ ] QR code scanning
- [ ] Basic computer vision (face detection)

---

### 12. Bluetooth Low Energy Mesh

**Status:** Not implemented  
**Priority:** LOW  
**Estimated Effort:** 3-4 weeks  

**Features:**
- [ ] BLE Mesh provisioning
- [ ] Mesh message flooding
- [ ] Node discovery
- [ ] Mesh OTA updates

---

### 13. Advanced Graphics

**Status:** LVGL basic integration complete  
**Priority:** LOW  
**Estimated Effort:** Ongoing  

**Future Enhancements:**
- [ ] Hardware acceleration (GPU/DMA)
- [ ] Vector graphics support
- [ ] Animation framework
- [ ] Custom widget library
- [ ] Theme system

---

### 14. File System Enhancements

**Status:** LittleFS working  
**Priority:** LOW  
**Estimated Effort:** 1 week  

**Features:**
- [ ] File compression (LZ4/Zlib)
- [ ] File encryption
- [ ] Symbolic links
- [ ] File watching/notifications
- [ ] Journaling for crash recovery

---

### 15. Developer Tools

**Status:** Minimal  
**Priority:** LOW  
**Estimated Effort:** Ongoing  

**Tools Needed:**
- [ ] WASM app debugger integration
- [ ] Performance profiler
- [ ] Memory leak detector
- [ ] Crash report collection
- [ ] Remote debugging over network

---