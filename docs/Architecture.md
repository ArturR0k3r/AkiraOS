# Architecture of AkiraOS (v1.4.x)

## 1. Introduction

AkiraOS is a modular, security-focused embedded operating system built on **Zephyr RTOS v4.2.1**. It leverages a **custom WebAssembly (WASM) runtime** based directly on **WAMR (wasm-micro-runtime)** to provide high-performance, sandboxed application execution on resource-constrained devices like ESP32-S3, C6, and STM32H7.

---

## 2. Design Principles

* **STUPID SIMPLE**: Direct calls, minimal abstraction, and zero unnecessary wrappers.
* **WASM-First**: All user logic is isolated in a secure WebAssembly sandbox.
* **Loose Coupling**: Strict separation between the Runtime, Connectivity, and Hardware layers.
* **Capability-Based Security**: Default-deny access model; hardware access is granted only via signed manifests.

---

## 3. Layered Stack Architecture

### 3.1 Application Layer (Trust Level 3)

* **WASM Apps**: Untrusted code compiled to WebAssembly (C/C++, Rust, AssemblyScript).
* **Isolation**: Apps operate in a linear memory sandbox, preventing access to kernel space or other apps.

### 3.2 Interface Layer (Native API Bridge)

* **Akira Native API**: A unified bridge that exports host functions to WASM.
* **Modules**:
* `akira_api_display`: Optimized 2D graphics and framebuffer control.
* `akira_api_rf`: Direct access to CC1101, LR1121, and NRF24.
* `akira_api_mesh`: Native bindings for the **AkiraMesh** protocol.
* `akira_api_input`: Abstracted HID, gamepad, and sensor inputs.



### 3.3 Runtime Layer (Custom Manager)

* **Akira Runtime**: Directly interfaces with WAMR C-API (`wasm_export.h`) for minimal overhead (<30KB footprint).
* **App Manager**: Supervisor for the app state machine (`LOADED`, `RUNNING`, `PAUSED`, `CRASHED`).
* **App Loader**: Atomic loading logic that fetches binaries from **Connectivity** or **Storage** into RAM/PSRAM.
* **Security Enforcer**: Validates app manifests and enforces capabilities before every native API call.

### 3.4 Connectivity Layer (Wireless & Transport)

* **Transports**: Bluetooth (BLE), WiFi (ESP32-native), USB (HID/MSC/CDC), and **AkiraMesh**.
* **Services**: Cloud client (CoAP/MQTT) for **AkiraHub** interaction and unified OTA updates.

### 3.5 Hardware Abstraction Layer (platform_hal)

* **Unified HAL**: A chip-agnostic layer providing a consistent interface for the OS core.
* **Driver Registry**: Dynamic or static registration of board-specific drivers.

---

## 4. System Flow & Interaction

1. **Deployment**: An app binary and its `manifest.json` arrive via **BLE** or **USB**.
2. **Staging**: The **App Loader** saves the binary to **Storage** (LittleFS/SD Card).
3. **Instantiation**: The **App Manager** requests the **Runtime** to load the app into memory.
4. **Execution**: **WAMR** starts the bytecode execution; the **Security Enforcer** monitors all outgoing API calls.

---

## 5. Security Model: Capability-Based Access

Every native function exported to WASM starts with a security check:

```c
if (!akira_security_check(current_app, CAP_DISPLAY_WRITE)) {
    return -EACCES; // Access denied if not in manifest
}

```

Capabilities are defined in the app's manifest and verified by the **App Manager** during the loading phase.

