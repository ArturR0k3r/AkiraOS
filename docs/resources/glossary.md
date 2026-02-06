# Glossary

## AkiraOS Terms

**AkiraRuntime**  
Custom WebAssembly runtime environment for managing WASM application lifecycle, security, and native API bridging.

**Capability**  
Permission bit granting access to specific system resources (display, sensors, network, etc.).

**Manifest**  
JSON or embedded WASM metadata declaring app capabilities, memory quota, and metadata.

**Transport Interface**  
Callback-based routing layer connecting network protocols to system consumers.

---

## WebAssembly

**AOT (Ahead-of-Time)**  
Pre-compilation of WASM to native machine code before execution.

**Linear Memory**  
Contiguous memory space accessible to WASM applications.

**WAMR**  
WebAssembly Micro Runtime - bytecode interpreter/compiler used by AkiraOS.

**WASI**  
WebAssembly System Interface - POSIX-like API (not used in AkiraOS).

**WAT**  
WebAssembly Text Format - human-readable representation of WASM.

---

## Zephyr/Embedded

**Device Tree**  
Hardware description format used by Zephyr for board configuration.

**HAL**  
Hardware Abstraction Layer - interface between drivers and applications.

**Kconfig**  
Configuration system for build-time options.

**MCUboot**  
Secure bootloader with OTA support and rollback protection.

**PSRAM**  
Pseudo-static RAM - external memory for heap expansion.

**West**  
Zephyr's meta-tool for workspace and dependency management.

---

## Networking

**BLE**  
Bluetooth Low Energy - power-efficient wireless protocol.

**GATT**  
Generic Attribute Profile - BLE data transfer protocol.

**HID**  
Human Interface Device - standard for input devices (keyboard, mouse).

**OTA**  
Over-the-Air - wireless firmware update mechanism.

---

## General

**RTOS**  
Real-Time Operating System - OS with deterministic scheduling.

**SoC**  
System on Chip - integrated circuit with CPU, memory, peripherals.

**UART**  
Universal Asynchronous Receiver-Transmitter - serial communication protocol.
