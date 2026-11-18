# AkiraOS — Copilot Instructions

AkiraOS is an embedded OS combining **Zephyr RTOS** with **WASM Micro Runtime (WAMR)** for sandboxed application execution on resource-constrained devices (ESP32-S3, nRF54L15, STM32, and etc.).

---

## Core Coding Principles (Follow These First)

1. **Use Maximum Available Zephyr API** — If Zephyr RTOS provides a solution, use it. Don't reinvent the wheel.
2. **KEEP IT SIMPLE (STUPID)** — Write the simplest code that solves the problem. One page max to explain.
3. **One Entity Per Source File** — Each `.c` file controls one module only (e.g., sensor, display, network).
4. **Readable > Clever** — Explicit names, comments explain **why** not **what**, avoid cryptic code.
5. **Fail Fast, Fail Loud** — Check inputs immediately, log errors with context, never silently ignore.
6. **Security by Default** — Every WASM native API guards with `AKIRA_CHECK_CAP_INLINE()`. Validate all inputs.
7. **Memory Awareness** — Always use `akira_malloc_buffer()`, never `k_malloc()` directly. Embedded devices have limited RAM.
8. **Single Responsibility Per Function** — One function = one job. No "and" in function names.
9. **Consistent Error Handling** — Follow Zephyr errno (`-EINVAL`, `-ENOMEM`, `-EIO`). Document return codes.
10. **Module Isolation** — Public APIs only; no direct access to internal static variables.
11. **Documentation Now, Not Later** — Every public function gets a doc comment before shipping.
12. **Test-Friendly Code** — Avoid global state, pass dependencies as arguments, separate logic from I/O.
13. **One Config Per Concern** — Each feature gets `CONFIG_AKIRA_<FEATURE>`. No magic numbers in code.
14. **Log at the Right Level** — `LOG_ERR()` errors, `LOG_WRN()` warnings, `LOG_INF()` important, `LOG_DBG()` debug.
15. **Git History Matters** — One logical change per commit, follow Conventional Commits format.

---

## Architecture Overview

```
WASM Apps → Runtime Core (akira_runtime.c) → Native Bridge (akira_export_api.c)
                                           → Security/Capability Guard (security.h)
                                           → PSRAM Allocator (lib/mem_helper.h)
                                           → WAMR Engine (modules/wasm-micro-runtime/)
Zephyr RTOS underlies all: filesystem, networking, GPIO, BLE, USB
```

- **`src/runtime/`** — App lifecycle, manifest parsing, security sandbox, module cache. `g_apps[AKIRA_MAX_WASM_INSTANCES]` is the global app registry.
- **`src/api/`** — Native function bridge; `akira_export_api.c` registers all WASM-callable C functions via `NativeSymbol[]`. Every API file is conditionally compiled (`CONFIG_AKIRA_WASM_*`).
- **`src/connectivity/`** — WiFi/HTTP, BLE, USB, OTA, mesh; accessed from WASM only through registered native APIs.
- **`src/lib/`** — Shared utilities: `mem_helper.h` (PSRAM-aware alloc), `simple_json.h`, `path_utils.h`.
- **`boards/`** — Per-board `.conf`/`.overlay` fragments. Custom boards have full DTS+defconfig in `boards/<boardname>/`.

---

- **Hardware Agnostic**: Code should work across all supported platforms with appropriate Kconfig flags
- **Native APIs**: Expose hardware features via controlled native APIs with capability checks
- **Modular Architecture**: Clear separation between radio abstraction, protocol managers, runtime, and shell integration
- **Error Handling**: Consistent use of Zephyr errno and comprehensive logging for all operations
- **Documentation**: Thorough code comments and architecture docs for maintainability and onboarding new contributors

---

## When You Don't Know

- **Zephyr API?** Check `https://docs.zephyrproject.org/latest/` first
- **WAMR API?** Check `https://github.com/bytecodealliance/wasm-micro-runtime/blob/main/doc/embed_wamr.md#execute-wasm-functions-in-multiple-threads`
- **Export Native API?** See `https://github.com/bytecodealliance/wasm-micro-runtime/blob/main/doc/export_native_api.md`
- **App Framework?** Check `https://github.com/bytecodealliance/wamr-app-framework/tree/main/app-framework`
- **Remote App Management?** See `https://github.com/bytecodealliance/wamr-app-framework/tree/e97860611e371d91d2ef25101b02b29719476a48/app-mgr#remote-application-management`
- **Spawn API?** Check `https://github.com/bytecodealliance/wasm-micro-runtime/blob/ff10b8693801e4cc7f8cf8b381a0da578513c4e8/doc/embed_wamr_spawn_api.md`
---

## When Generating Code

### Module Header (Every `.c` File)

```c
/*
 * Copyright (c) 2025 AkiraOS Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define LOG_MODULE_NAME akira_<subsystem>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(akira_<subsystem>, CONFIG_AKIRA_LOG_LEVEL);

/**
 * @file akira_<subsystem>.c
 * @brief Brief description of module purpose
 */
```
---
### Error Handling (Always)

```c
// Check return values immediately
int ret = akira_init();
if (ret < 0) {
    LOG_ERR("Init failed: %d", ret);
    return ret;
}

// Validate pointers and bounds
if (!data || len > MAX_SIZE) {
    LOG_ERR("Invalid input: data=%p len=%u", data, len);
    return -EINVAL;
}
```
---
### Logging

```c
LOG_ERR("Error: %d", code);           // Errors — always log
LOG_WRN("Warning: out of bounds");    // Warnings
LOG_INF("System started");            // Important state changes
LOG_DBG("Debug: sensor_id=%u", id);   // Development (stripped in prod)
```

---

## Error Codes (Zephyr errno)

- `0` — Success
- `-EINVAL` — Invalid argument
- `-ENOMEM` — Out of memory
- `-EACCES` — Permission denied (capability check failed)
- `-EIO` — Hardware/communication error
- `-ENOTSUP` — Feature not supported or disabled
- `-ETIMEDOUT` — Operation timeout

---

## Build & Run

```bash
# Quick dev loop — native simulator (no hardware needed)
./build.sh                                    # builds + runs native_sim
./build.sh -b native_sim                      # explicit

# Hardware targets
./build.sh -b akiraconsole -bl y -r all       # MCUboot + app, flash both
./build.sh -b akiraconsole -r a               # flash app only (bootloader exists)
./build.sh -b esp32s3_super_mini_esp32s3_procpu

# Pristine rebuild (raw west)
cd .. && unset ZEPHYR_BASE
west build --pristine -b <board> AkiraOS/ -d build -- -DMODULE_EXT_ROOT=AkiraOS/

# Build WASM sample apps (requires WASI SDK at /opt/wasi-sdk or $WASI_SDK)
cd wasm_sample && ./build_wasm_apps.sh        # all apps → wasm_sample/bin/
./build_wasm_apps.sh hello_world              # single app
```

`MODULE_EXT_ROOT` **must** point to the workspace root when invoking west directly — `build.sh` handles this automatically.

## WASM App & Manifest Pattern

Each app pairs a `.c` source with a `.json` manifest (or embeds one in the WASM custom section `"akira-manifest"`):

```json
{ "name": "my_app", "version": "1.0.0",
  "capabilities": ["display.write", "gpio.read"],
  "memory_quota": 65536 }
```

Capability strings map to `AKIRA_CAP_*` bitmasks in `src/runtime/security.h` (e.g., `AKIRA_CAP_DISPLAY_WRITE = 1<<0`). The runtime merges embedded + external manifests; external has additive authority.

## Adding a Native API

1. Implement in `src/api/akira_<subsystem>_api.c`, following the pattern of existing APIs (e.g., `akira_display_api.c`). Each function must take an `exec_env` pointer as the first argument for capability checks.
2. Register in `akira_export_api.c` `NativeSymbol[]` with a WAMR type signature (e.g., `"(ii)i"` = two i32 args, i32 return).

## Memory Allocation Rules

- **Always** use `akira_malloc_buffer()` / `akira_free_buffer()` from `<lib/mem_helper.h>` for any runtime-managed allocation. It prefers PSRAM, falls back to SRAM — never call `k_malloc` directly for buffers that may be large.
- Use `akira_malloc_buffer_ex(..., &mem_source)` to know which memory region was used.
- WAMR's global heap is pre-allocated at init; size via `CONFIG_WAMR_HEAP_SIZE` (default 256 KB).

## Key Configuration Flags

| Flag | Purpose |
|------|---------|
| `CONFIG_AKIRA_WASM_RUNTIME` | Enables WAMR; without it all runtime functions return `-ENOTSUP` |
| `CONFIG_WAMR_AOT_SUPPORT` | AOT mode - so the apps are compiled for a specific target architecture |
| `CONFIG_AKIRA_PSRAM` | Activates PSRAM heap; required for WASM apps >128 KB |
| `CONFIG_AKIRA_APP_MANAGER` | Persistent app install/uninstall via filesystem |

## Code Conventions

- All WAMR-dependent code is wrapped in `#ifdef CONFIG_AKIRA_WASM_RUNTIME`; stubs at the `#else` branch must return `-ENOTSUP`.
- `LOG_MODULE_REGISTER(<name>, CONFIG_AKIRA_LOG_LEVEL)` at the top of every `.c` file.
- App slot lookup: prefer `instance_map_get(inst)` (O(1)) over linear scan of `g_apps[]`.
- Error returns follow Zephyr errno convention (`-EINVAL`, `-ENOMEM`, `-ENOTSUP`, etc.).
- Board-specific tuning goes in `boards/<board>.conf` or `boards/<board>.overlay`; never in `prj.conf`.

---

### Git Commits

Format: `<type>(<scope>): <description>`

**Types:** `feat`, `fix`, `docs`, `test`, `refactor`, `chore`

**Examples:**
```
feat(display): add 16-bit color support
fix(sensor): resolve timeout on invalid ID
docs(api): update capability reference
test(core): add memory allocator tests
refactor(runtime): simplify app registry
```