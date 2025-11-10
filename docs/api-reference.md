# AkiraOS System Logic & Architecture

## Main System File
`src/akiraos.c` orchestrates all subsystems:
- Registers and starts core services (graphics, input, network, storage, audio, security, UI)
- Initializes drivers (display, buttons, etc.)
- Sets up OTA, Bluetooth, Shell, Settings
- Initializes event system and subscribes handlers
- Initializes WASM and OCRE runtimes
- Launches default apps (menu, shell, etc.)

## Modular Subsystems
- **System Services**: `service_manager.c/h` — register, start, stop, query services
- **Event System**: `event_system.c/h` — publish/subscribe events
- **Process Management**: `process_manager.c/h` — launch/stop/status/list apps
- **WASM App Manager**: `wasm_app_manager.c/h` — upload/update/list WASM modules
- **OCRE Runtime**: `ocre_runtime.c/h` — load/start/stop/list native/WASM apps

## Integration Example
See `src/akiraos.c` for how all modules are initialized and connected.

## Extending AkiraOS
- Add new services by implementing and registering with `service_manager`
- Add new event types and handlers via `event_system`
- Add new apps (native or WASM) via `process_manager` and `ocre_runtime`
- Add new drivers or host APIs for WASM via WAMR integration

## Strategic Positioning
See the main `README.md` for business, community, and hardware strategy.

## System Services Architecture
- Modular service manager (`service_manager.c/h`) for core OS features.
- Each service is an independent module with API and lifecycle (init, start, stop, status).

## Event System
- Central event bus (`event_system.c/h`) for inter-module communication.
- Supports event publishing, subscription, and filtering.
- Example events: button press, OTA progress, network status, app install, shell command, BLE connect/disconnect.

## Process Management
- Lightweight process/task manager (`process_manager.c/h`) for running apps and services.
- Supports launching, stopping, monitoring, and resource tracking.
- Integrates with WASM runtime for sandboxed app execution.

## WASM Apps Upload & Update
- WASM app manager (`wasm_app_manager.c/h`) for uploading, updating, and listing WASM apps.
- Upload via OTA (WebServer, BLE, USB, Cloud), validate and register apps.
- Version management and secure update with rollback support.

## OCRE and WASM Integration
- OCRE runtime (`ocre_runtime.c/h`) manages native and WASM apps.
- Loads and executes WASM apps using WASM-Micro-Runtime.
- Exposes system APIs to WASM apps via host functions.
- Unified app management for native and WASM apps.

---

### File Overview
- `src/services/service_manager.c/h`: System service manager
- `src/services/event_system.c/h`: Event system
- `src/services/process_manager.c/h`: Process manager
- `src/services/wasm_app_manager.c/h`: WASM app upload/update manager
- `src/services/ocre_runtime.c/h`: OCRE runtime for native/WASM apps

---

### Example Usage

**Registering a Service:**
```c
akira_service_t wifi_service = {
    .name = "wifi",
    .init = wifi_init,
    .start = wifi_start,
    .stop = wifi_stop,
    .status = wifi_status,
    .running = false
};
service_manager_register(&wifi_service);
```

**Publishing an Event:**
```c
akira_event_t event = {
    .type = EVENT_BUTTON_PRESS,
    .data = &button_id,
    .data_size = sizeof(button_id)
};
event_system_publish(&event);
```

**Launching a WASM App:**
```c
wasm_app_upload("game.wasm", binary, size, 1);
akira_process_t proc = {
    .name = "game",
    .type = PROCESS_TYPE_WASM,
    .entry = binary,
    .running = false
};
process_manager_launch(&proc);
```

**Loading an App in OCRE:**
```c
ocre_app_t app = {
    .name = "game",
    .type = OCRE_APP_WASM,
    .entry = binary,
    .running = false
};
ocre_runtime_load_app(&app);
ocre_runtime_start_app(app.pid);
```

---

# OCRE Integration in AkiraOS

AkiraOS uses [OCRE](https://github.com/project-ocre/ocre-runtime) for all container and WASM app management. The following APIs are available:

## OCRE Runtime API
- `ocre_runtime_load_app(const char *name, const void *binary, size_t size)` — Upload a new container or WASM app
- `ocre_runtime_start_app(const char *name)` — Start a container/app
- `ocre_runtime_stop_app(const char *name)` — Stop a container/app
- `ocre_runtime_list_apps(akira_ocre_container_t *out_list, int max_count)` — List all containers/apps

All operations are delegated to OCRE's runtime APIs, providing secure, lightweight management for embedded devices.

## Example
```c
// Upload and start a WASM app
ocre_runtime_load_app("game", binary, size);
ocre_runtime_start_app("game");
```

## Build and Integration Notes
- OCRE is included as a west module and linked via CMake.
- No stub logic remains; all app lifecycle management is handled by OCRE.

For more details, see the individual header files in `src/services/`.
