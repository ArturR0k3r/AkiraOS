AkiraOS Runtime Core (v1.4.x)

The primary execution layer for WebAssembly applications based on WAMR (wasm-micro-runtime).

1. High-Level Architecture

The Runtime serves as the central orchestration node of AkiraOS, isolating user-space applications from the Zephyr RTOS 4.3.0 kernel. It operates strictly through defined interfaces, ensuring that sandboxed code cannot access hardware drivers or kernel memory directly.

2. Core Components

2.1 Akira Runtime (WAMR Core)

Source File: src/runtime/akira_runtime.c

Role: Direct integration of the WAMR iWasm engine.

Design Logic:

Memory Management: Initializes a global heap within PSRAM for the WebAssembly environment.

Native Symbol Registration: Maintains the NativeSymbol table for the Bridge API.

Execution Environment: Handles the creation, management, and destruction of wasm_exec_env_t.

Strict Protocol: No abstraction macros or wrappers. Uses only native WAMR C-API calls to ensure maximum performance and minimal overhead.

2.2 App Loader

Source File: src/runtime/loader/app_loader.c

Role: Binary preparation and validation for execution.

Design Logic:

Data Sourcing: Reads data chunks from the fs_manager (Internal Flash/SD Card) or direct network streams (Connectivity Layer).

Validation: Performs strict checks on WASM magic bytes, headers, and CRC32 integrity.

Buffer Allocation: Manages memory allocation for the binary and passes the validated pointer to the runtime core.

2.3 App Manager (Supervisor)

Source File: src/runtime/manager/app_manager.c

Role: Application lifecycle orchestration and supervision.

Design Logic:

State Machine: Implements a strict FSM: IDLE -> LOADING -> STARTING -> RUNNING -> STOPPING -> CRASHED.

Fault Tolerance: Handles WASM Traps and exceptions, ensuring the system remains stable even if an application fails.

Concurrency (v1.5.0+): Manages time-slicing and resource quotas if multiple applications are initialized.

2.4 Security (Capability Guard)

Source File: src/runtime/security/security.c

Role: System-level call filtering and permission enforcement.

Design Logic:

Manifest Processing: Parses the manifest.json file during the application loading phase.

Capability Mapping: Stores a bitmask of granted permissions for each unique app_id.

Real-time Enforcement: The akira_security_check(exec_env, capability) function is called at the entry point of every Native API to verify access rights.

3. Interaction Flow

Manager issues a command to the Loader to fetch a specific application binary.

Loader allocates memory, validates the binary, and stages it for the runtime.

Security parses the associated manifest and caches the application's capabilities.

Runtime instantiates the module, creates the execution environment, and begins execution.

4. Native Modules

Internal runtime modules (e.g., Logging, System Time, Internal IPC) are registered directly within this layer. These modules provide essential services to the WASM sandbox without exposing underlying hardware peripherals.

5. Design Philosophy: "One Module, One Functionality"

Decoupling: Each component is logically separated; the Loader does not know about the internal state of the Manager.

Transparency: All WAMR interactions are explicit, facilitating easier debugging and performance profiling.

Security by Default: No hardware access is possible without a verified capability bit in the security bitmask.