# AkiraOS Core System

This directory contains the core system management infrastructure that orchestrates all AkiraOS subsystems.

## Architecture

```
┌─────────────────────────────────────────┐
│           main.c (~70 lines)            │
│  (Simplified entry point)               │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│       system_manager.c/h                │
│  • Orchestrates initialization          │
│  • Manages subsystem lifecycle          │
│  • Publishes system events              │
└──────────────┬──────────────────────────┘
               │
               ├────────────────────┬──────────────────┐
               ▼                    ▼                  ▼
┌──────────────────────┐ ┌──────────────────┐ ┌──────────────────┐
│  hardware_manager    │ │  network_manager │ │    event_bus     │
│  • HAL init          │ │  • WiFi          │ │  • Pub/sub       │
│  • Driver registry   │ │  • Bluetooth     │ │  • Loose coupling│
│  • Button handlers   │ │  • USB           │ │  • System events │
│  • Display           │ │  • Cloud client  │ │                  │
└──────────────────────┘ └──────────────────┘ └──────────────────┘
               │                    │                  ▲
               └────────────────────┴──────────────────┘
                          (via events)
```

## Components

### 1. Event Bus (`event_bus.c/h`)
- **Purpose**: Decouples subsystems via publish/subscribe pattern
- **Features**:
  - 32 subscriber slots
  - Type-safe event system
  - Mutex-protected for thread safety
  - Events: SYSTEM, NETWORK, STORAGE, OTA, APP, SETTINGS, HARDWARE, POWER
- **Usage**:
  ```c
  // Subscribe
  event_bus_subscribe(EVENT_NETWORK_CONNECTED, my_callback, user_data);
  
  // Publish
  system_event_t event = {
      .type = EVENT_NETWORK_CONNECTED,
      .timestamp = k_uptime_get(),
      .data.network.type = NETWORK_TYPE_WIFI
  };
  event_bus_publish(&event);
  ```

### 2. Init Table (`init_table.c/h`)
- **Purpose**: Priority-based subsystem initialization
- **Features**:
  - 8 priority levels (EARLY → LATE)
  - Automatic sorting by priority
  - Required vs optional subsystems
  - Kconfig-driven enablement
  - Detailed initialization logging
- **Priorities**:
  - `INIT_PRIORITY_EARLY` (0): HAL, event bus
  - `INIT_PRIORITY_PLATFORM` (10): Driver registry, hardware
  - `INIT_PRIORITY_DRIVERS` (20): Device drivers
  - `INIT_PRIORITY_STORAGE` (30): Filesystems, settings
  - `INIT_PRIORITY_NETWORK` (40): WiFi, BT, USB
  - `INIT_PRIORITY_SERVICES` (50): App manager, OCRE, OTA
  - `INIT_PRIORITY_APPS` (60): User applications
  - `INIT_PRIORITY_LATE` (70): Shell, final setup

### 3. System Manager (`system_manager.c/h`)
- **Purpose**: Main orchestrator for all subsystems
- **Features**:
  - Registers all subsystems with init table
  - Publishes boot/ready/shutdown events
  - Coordinates initialization phases
  - Provides system status API
- **Lifecycle**:
  1. `system_manager_init()` - Initialize all subsystems
  2. `system_manager_start()` - Start runtime services
  3. `system_manager_is_ready()` - Check status
  4. `system_manager_shutdown()` - Graceful shutdown

### 4. Hardware Manager (`hardware_manager.c/h`)
- **Purpose**: Coordinates hardware initialization
- **Features**:
  - Initializes HAL (`akira/hal/`)
  - Initializes driver registry
  - Configures button handlers with event publishing
  - Initializes display (if configured)
  - Initializes LVGL UI (if configured)
- **Dependencies**: Leverages existing HAL and driver_registry

### 5. Network Manager (`network_manager.c/h`)
- **Purpose**: Coordinates network connectivity
- **Features**:
  - Initializes WiFi manager
  - Initializes Bluetooth manager
  - Initializes USB manager
  - Initializes cloud client
  - Publishes connectivity events
  - Tracks connection state
- **Dependencies**: Uses existing connectivity/ layer managers

## Integration

### Adding a New Subsystem

1. **Create init function** in your subsystem:
   ```c
   int my_subsystem_init(void) {
       // Initialize your subsystem
       return 0;
   }
   ```

2. **Register in system_manager.c**:
   ```c
   init_table_register("My Subsystem", INIT_PRIORITY_SERVICES,
                      my_subsystem_init, false, 
                      IS_ENABLED(CONFIG_MY_SUBSYSTEM));
   ```

3. **Publish events** when appropriate:
   ```c
   system_event_t event = {
       .type = EVENT_MY_EVENT,
       .timestamp = k_uptime_get()
   };
   event_bus_publish(&event);
   ```

4. **Subscribe to events** you care about:
   ```c
   event_bus_subscribe(EVENT_NETWORK_CONNECTED, on_network_ready, NULL);
   ```

### Migrating Legacy Code

**Before** (in main.c):
```c
ret = my_subsystem_init();
if (ret < 0) {
    LOG_ERR("Init failed");
    return ret;
}
```

**After** (in system_manager.c):
```c
init_table_register("My Subsystem", INIT_PRIORITY_SERVICES,
                   my_subsystem_init, false, true);
```

## Benefits

1. **Decoupling**: Modules don't directly depend on each other
2. **Configurability**: Kconfig controls what gets initialized
3. **Maintainability**: Clear separation of concerns
4. **Debuggability**: Detailed initialization logging
5. **Extensibility**: Easy to add new subsystems
6. **Testability**: Can test subsystems in isolation

## File Structure

```
src/core/
├── README.md                 # This file
├── event_bus.h/.c           # Event pub/sub system
├── init_table.h/.c          # Priority-based init
├── system_manager.h/.c      # Main orchestrator
├── hardware_manager.h/.c    # Hardware coordination
└── network_manager.h/.c     # Network coordination
```

## Configuration

All core components respect Kconfig settings:

- `CONFIG_AKIRA_LOG_LEVEL` - Logging verbosity
- `CONFIG_WIFI` - Enable WiFi support
- `CONFIG_BT` - Enable Bluetooth support
- `CONFIG_USB_DEVICE_STACK` - Enable USB support
- `CONFIG_AKIRA_DISPLAY` - Enable display support
- `CONFIG_AKIRA_SETTINGS` - Enable settings system
- `CONFIG_AKIRA_STORAGE_FATFS` - Enable storage
- `CONFIG_AKIRA_APP_MANAGER` - Enable app manager
- `CONFIG_AKIRA_SHELL` - Enable shell

## Future Enhancements

- [ ] Event filtering and priorities
- [ ] Event history/replay for debugging
- [ ] Dynamic subsystem loading/unloading
- [ ] Init table visualization tool
- [ ] Performance metrics per subsystem
- [ ] Subsystem health monitoring
- [ ] Automatic dependency resolution

## References

- See `docs/ARCHITECTURE.md` for high-level architecture
- See `docs/AkiraOS.md` for system overview
- See `src/akira/hal/` for HAL implementation
- See `src/drivers/driver_registry.c` for driver loading
