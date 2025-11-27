# AkiraOS v2.0 Implementation TODO

## Current State Analysis

### ✅ IMPLEMENTED (Working)
- Basic OCRE runtime integration (`ocre_runtime.c`)
- Service manager framework (`service_manager.c`)
- Event system (publish/subscribe) (`event_system.c`)
- OTA Manager with MCUboot (`ota_manager.c`)
- Web server for OTA uploads (`web_server.c`)
- Akira HAL for ESP32/ESP32-S3/native_sim (`akira_hal.c`)
- Display driver ILI9341 (`display_ili9341.c`)
- nRF24L01 driver (`nrf24l01.c`)
- LSM6DS3 sensor driver (`lsm6ds3.c`)
- INA219 power sensor (`ina219.c`)
- Shell with basic commands (`akira_shell.c`)
- Button input system (`akira_buttons.c`)
- Settings storage (`settings.c`)
- Bluetooth manager skeleton (`bluetooth_manager.c`)
- WASM app manager wrapper (`wasm_app_manager.c`)

### ⚠️ PARTIAL / STUB
- Bluetooth manager (skeleton only, no BLE stack)
- Graphics service (stub in `akiraos.c`)
- Audio service (stub)
- Network service (stub)
- Storage service (stub)
- Security service (stub)
- UI service (stub)

### ❌ NOT IMPLEMENTED (Required for v2.0)
- Trust model / security levels
- App signing & verification
- Capability-based permissions
- Resource quotas & scheduling
- IPC (message bus, shared memory, RPC)
- RF driver framework (unified API)
- LR1121/CC1101/SX1276/RFM69 drivers
- Power manager
- Akira API exports for WASM
- nRF52/nRF53/nRF91 platform support
- STM32 platform support
- Shell commands for containers/RF/sensors

---

## Implementation Plan

### Phase 1: Fix Current Build & Core Stability
**Priority: CRITICAL**

- [ ] **1.1** Fix ESP32-S3 build failure (Exit Code 1)
- [ ] **1.2** Verify OCRE module integration compiles
- [ ] **1.3** Test native_sim build and run
- [ ] **1.4** Clean up unused stub code in `akiraos.c`

---

### Phase 2: Security & Trust Model
**Priority: HIGH**

#### 2.1 Trust Levels
- [ ] Define `enum akira_trust_level` (KERNEL=0, SYSTEM=1, TRUSTED=2, USER=3)
- [ ] Add trust level field to container structure
- [ ] Implement trust level checks in OCRE wrapper

#### 2.2 App Signing
- [ ] Create `src/security/app_signing.c`
- [ ] Implement RSA-2048 or Ed25519 signature verification
- [ ] Add root CA storage in NVS
- [ ] Validate app signature before loading in `ocre_runtime_load_app()`

#### 2.3 Capability System
- [ ] Define capability enum (`CAP_DISPLAY_WRITE`, `CAP_RF_TRANSCEIVE`, etc.)
- [ ] Create manifest parser (JSON or binary format)
- [ ] Store granted capabilities per container
- [ ] Add capability check wrapper for all Akira API calls

---

### Phase 3: Resource Management
**Priority: HIGH**

#### 3.1 Memory Quotas
- [ ] Create `src/services/resource_manager.c`
- [ ] Track per-container memory usage
- [ ] Enforce `max_memory_kb` from manifest
- [ ] Add OOM handler per container

#### 3.2 CPU Scheduling
- [ ] Implement CPU time tracking per container
- [ ] Add `max_cpu_percent` enforcement
- [ ] Create throttling mechanism for quota exceeded

#### 3.3 Storage Quotas
- [ ] Track LittleFS usage per container
- [ ] Implement storage quota enforcement
- [ ] Add quota shell commands

#### 3.4 Watchdog
- [ ] Add 5-second watchdog per container
- [ ] Kill containers that block too long
- [ ] Log watchdog events

---

### Phase 4: Inter-Process Communication (IPC)
**Priority: HIGH**

#### 4.1 Message Queue
- [ ] Create `src/services/ipc_msgq.c`
- [ ] Implement `ocre_publish_event()` / `ocre_subscribe_channel()`
- [ ] Add wildcard channel matching (`sensor/*`)

#### 4.2 Shared Memory
- [ ] Create `src/services/ipc_shm.c`
- [ ] Implement `ocre_shm_create()` / `ocre_shm_attach()`
- [ ] Add ACL enforcement for shared regions

#### 4.3 RPC Proxy
- [ ] Create `src/services/ipc_rpc.c`
- [ ] Implement `ocre_rpc_call()` for sync calls
- [ ] Export function registration for apps

---

### Phase 5: Akira API (WASM Exports)
**Priority: HIGH**

#### 5.1 Display API
- [ ] Create `src/api/akira_display_api.c`
- [ ] Export: `akira_display_clear`, `akira_display_pixel`, `akira_display_rect`, `akira_display_text`, `akira_display_flush`
- [ ] Register exports with WAMR

#### 5.2 Input API
- [ ] Create `src/api/akira_input_api.c`
- [ ] Export: `akira_input_read_buttons`, `akira_input_set_callback`

#### 5.3 RF API
- [ ] Create `src/api/akira_rf_api.c`
- [ ] Export: `akira_rf_init`, `akira_rf_send`, `akira_rf_receive`, `akira_rf_set_frequency`, `akira_rf_set_power`

#### 5.4 Sensor API
- [ ] Create `src/api/akira_sensor_api.c`
- [ ] Export: `akira_sensor_read`, `akira_sensor_read_imu`, `akira_sensor_read_env`

#### 5.5 Storage API
- [ ] Create `src/api/akira_storage_api.c`
- [ ] Export: `akira_storage_read`, `akira_storage_write`, `akira_storage_delete`, `akira_storage_list`

#### 5.6 Network API
- [ ] Create `src/api/akira_network_api.c`
- [ ] Export: `akira_http_get`, `akira_http_post`, `akira_mqtt_publish`, `akira_mqtt_subscribe`

---

### Phase 6: RF Driver Framework
**Priority: MEDIUM**

#### 6.1 Unified RF Interface
- [ ] Create `src/drivers/rf_framework.h` with `akira_rf_driver` struct
- [ ] Define common API: `init`, `deinit`, `tx`, `rx`, `set_frequency`, `set_power`, `get_rssi`

#### 6.2 LR1121 Driver
- [ ] Create `src/drivers/lr1121.c`
- [ ] Implement LoRa and GFSK modes
- [ ] Add frequency hopping support

#### 6.3 CC1101 Driver
- [ ] Create `src/drivers/cc1101.c`
- [ ] Implement FSK/GFSK/OOK modes
- [ ] Add packet handling

#### 6.4 SX1276 Driver
- [ ] Create `src/drivers/sx1276.c`
- [ ] Implement LoRa mode
- [ ] Add spread factor configuration

#### 6.5 RFM69 Driver
- [ ] Create `src/drivers/rfm69.c`
- [ ] Implement FSK mode

---

### Phase 7: Power Manager
**Priority: MEDIUM**

- [ ] Create `src/power/power_manager.c`
- [ ] Implement power modes: Active, Idle, Light Sleep, Deep Sleep, Hibernate
- [ ] Add `akira_pm_set_mode()`, `akira_pm_wake_on_gpio()`, `akira_pm_wake_on_timer()`
- [ ] Implement battery level reading via INA219
- [ ] Add power policy aggregation (apps request preferences)
- [ ] Integrate with ESP32 sleep APIs

---

### Phase 8: Additional Display Drivers
**Priority: LOW**

- [ ] ST7789 driver (`src/drivers/st7789.c`)
- [ ] SSD1306 OLED driver (`src/drivers/ssd1306.c`)
- [ ] E-Paper driver (`src/drivers/epaper.c`)
- [ ] Create display abstraction layer

---

### Phase 9: Additional Sensor Drivers
**Priority: LOW**

- [ ] BME280 driver (`src/drivers/bme280.c`)
- [ ] MPU6050 driver (`src/drivers/mpu6050.c`)
- [ ] BMP280 driver (`src/drivers/bmp280.c`)
- [ ] VEML7700 driver (`src/drivers/veml7700.c`)

---

### Phase 10: Platform Support
**Priority: LOW (Future)

#### 10.1 Nordic nRF52840
- [ ] Create `boards/nrf52840dk.conf`
- [ ] Create `boards/nrf52840dk.overlay`
- [ ] Test BLE stack integration

#### 10.2 Nordic nRF5340
- [ ] Create `boards/nrf5340dk.conf`
- [ ] Handle dual-core architecture

#### 10.3 Nordic nRF9160
- [ ] Create `boards/nrf9160dk.conf`
- [ ] Add LTE-M/NB-IoT support

#### 10.4 STM32 Family
- [ ] Create `boards/nucleo_f446re.conf`
- [ ] Create `boards/nucleo_wb55rg.conf`

---

### Phase 11: Shell Commands
**Priority: MEDIUM**

#### 11.1 Container Commands
- [ ] `ocre list` — list all containers
- [ ] `ocre start <name>` — start container
- [ ] `ocre stop <name>` — stop container
- [ ] `ocre status <name>` — show container status
- [ ] `ocre resources <name>` — show resource usage
- [ ] `ocre logs <name>` — show container logs

#### 11.2 RF Commands
- [ ] `rf list` — list available RF chips
- [ ] `rf init <chip>` — initialize RF
- [ ] `rf send <hex>` — send raw packet
- [ ] `rf recv` — receive packet
- [ ] `rf freq <hz>` — set frequency
- [ ] `rf power <dbm>` — set TX power

#### 11.3 Sensor Commands
- [ ] `sensor list` — list available sensors
- [ ] `sensor read <type>` — read sensor value
- [ ] `sensor stream <type>` — stream sensor data

---

### Phase 12: Bluetooth Manager
**Priority: MEDIUM**

- [ ] Initialize Zephyr BLE stack
- [ ] Create GATT services for:
  - [ ] OTA data transfer
  - [ ] Shell access (Nordic UART Service)
  - [ ] App management
- [ ] Implement HID keyboard/mouse profiles
- [ ] Add device simulation (change advertised name/type)

---

### Phase 13: Graphics Engine
**Priority: LOW (Future)

- [ ] Create `src/graphics/graphics_engine.c`
- [ ] Implement framebuffer management
- [ ] Add 2D primitives (lines, circles, polygons)
- [ ] Add sprite/tile support
- [ ] Add font rendering engine

---

### Phase 14: Audio Manager
**Priority: LOW (Future)

- [ ] Create `src/audio/audio_manager.c`
- [ ] Implement I2S output
- [ ] Add PCM/WAV playback
- [ ] Add basic synthesis (beeps, tones)

---

## File Structure (New Files to Create)

```
src/
├── api/
│   ├── akira_api.h           # All WASM export declarations
│   ├── akira_display_api.c
│   ├── akira_input_api.c
│   ├── akira_rf_api.c
│   ├── akira_sensor_api.c
│   ├── akira_storage_api.c
│   └── akira_network_api.c
├── security/
│   ├── app_signing.c
│   ├── app_signing.h
│   ├── capability.c
│   └── capability.h
├── power/
│   ├── power_manager.c
│   └── power_manager.h
├── drivers/
│   ├── rf_framework.h
│   ├── lr1121.c
│   ├── lr1121.h
│   ├── cc1101.c
│   ├── cc1101.h
│   ├── sx1276.c
│   ├── sx1276.h
│   ├── rfm69.c
│   ├── rfm69.h
│   ├── st7789.c
│   ├── st7789.h
│   ├── ssd1306.c
│   ├── ssd1306.h
│   ├── bme280.c
│   ├── bme280.h
│   ├── mpu6050.c
│   └── mpu6050.h
└── services/
    ├── resource_manager.c
    ├── resource_manager.h
    ├── ipc_msgq.c
    ├── ipc_shm.c
    ├── ipc_rpc.c
    └── ipc.h
```

---

## Estimated Effort

| Phase | Effort | Dependencies |
|-------|--------|--------------|
| Phase 1 | 1 day | None |
| Phase 2 | 1 week | Phase 1 |
| Phase 3 | 1 week | Phase 2 |
| Phase 4 | 1 week | Phase 3 |
| Phase 5 | 2 weeks | Phase 4 |
| Phase 6 | 2 weeks | Phase 1 |
| Phase 7 | 1 week | Phase 1 |
| Phase 8 | 3 days | Phase 1 |
| Phase 9 | 3 days | Phase 1 |
| Phase 10 | 2 weeks | Phase 1 |
| Phase 11 | 1 week | Phase 5, 6 |
| Phase 12 | 1 week | Phase 1 |
| Phase 13 | 2 weeks | Phase 5 |
| Phase 14 | 1 week | Phase 1 |

**Total: ~10-12 weeks for full v2.0**

---

## Quick Wins (Can Do Now)

1. **Fix build** — resolve Exit Code 1
2. **Add shell commands** for existing OCRE container management
3. **Implement power manager** — ESP32 sleep modes
4. **Complete Bluetooth manager** — basic BLE advertising
5. **Add LR1121/CC1101 drivers** — hardware already defined

---

## Notes

- OCRE is already integrated but only basic container lifecycle works
- Security model is the biggest gap — no signing, no capabilities
- RF framework is completely missing — only nRF24L01 exists
- Power management is absent — no sleep modes
- WASM apps cannot access hardware yet — Akira API exports missing
