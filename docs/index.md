# AkiraOS Documentation

<div align="center">

**High-Performance WebAssembly OS for Embedded Systems**

[![Version](https://img.shields.io/badge/version-1.4.8-blue.svg)](../VERSION)
[![Zephyr](https://img.shields.io/badge/Zephyr-4.3.0-green.svg)](https://github.com/zephyrproject-rtos/zephyr)
[![WAMR](https://img.shields.io/badge/WAMR-2.0-purple.svg)](https://github.com/bytecodealliance/wasm-micro-runtime)

*Production-ready embedded OS combining Zephyr RTOS with WebAssembly sandboxed execution.*

</div>

---

## 🚀 Quick Start

**New to AkiraOS?** Get up and running in under 30 minutes:

1. **[Installation Guide](getting-started/installation.md)** - Set up development environment
2. **[Build Your First App](getting-started/first-app.md)** - Hello World in WASM
3. **[SDK Best Practices](development/best-practices.md)** - Write better apps from day one
4. **[Troubleshooting](getting-started/troubleshooting.md)** - Common issues and fixes

**Already familiar?** Jump to:
- [Architecture Documentation](architecture/) - System design deep dive
- [API Reference](api-reference/) - Complete WASM API docs
- [SDK API Reference](development/sdk-api-reference.md) - Full Akira SDK function reference
- [Platform Guides](platform/) - Board-specific setup

---

## 📖 What is AkiraOS?

AkiraOS is a production-ready embedded operating system that enables **secure, dynamic application execution** on resource-constrained devices through WebAssembly.

### Core Features

✅ **Zero-Trust Execution** - WASM sandboxing + capability-based access control  
✅ **Real-Time Performance** - Zephyr RTOS with <60ns native call overhead  
✅ **OTA Updates** - MCUboot secure firmware updates with rollback  
✅ **Multi-Platform** - ESP32, nRF54, STM32, native simulation  
✅ **Modular Connectivity** - WiFi, Bluetooth, USB, mesh (future)  
✅ **Rich SDK** - Display, input, sensors, RF, storage, networking, and system APIs  

### Key Metrics

| Metric | Value |
|--------|-------|
| Native Call Overhead | ~60ns |
| WASM Load Time (100KB) | ~80ms |
| Concurrent Apps | 4 instances |
| Memory Per App | 64-128KB configurable |
| Boot Time | ~500ms (ESP32-S3) |

[Learn more about the architecture →](architecture/)

---

## 📚 Documentation Sections

### [Getting Started](getting-started/)
**Install, build, and deploy your first WASM application.**

- [Installation Guide](getting-started/installation.md) - Environment setup
- [First App Tutorial](getting-started/first-app.md) - Hello World
- [Troubleshooting](getting-started/troubleshooting.md) - Common issues

---

### [Architecture](architecture/)
**Deep dive into system design, components, and data flows.**

- [System Overview](architecture/system-overview.md) - Complete architecture
- [Connectivity Layer](architecture/connectivity.md) - Network protocols
- [AkiraRuntime](architecture/runtime.md) - WASM execution environment
- [Security Model](architecture/security.md) - Capability system
- [Data Flow](architecture/data-flow.md) - End-to-end diagrams

---

### [API Reference](api-reference/)
**Complete native API documentation for WASM applications.**

- [API Overview](api-reference/) - Quick reference
- [Native API](api-reference/native-api.md) - All functions
- [Manifest Format](api-reference/manifest-format.md) - App metadata
- [Error Codes](api-reference/error-codes.md) - Return values

---

### [SDK Developer Guide](development/)
**Complete Akira SDK documentation for WASM app development.**

- [SDK API Reference](development/sdk-api-reference.md) - All SDK functions with examples
- [Best Practices](development/best-practices.md) - Patterns for efficient, maintainable code
- [SDK Troubleshooting](development/sdk-troubleshooting.md) - Debug common app issues

---

### [Platform Guides](platform/)
**Board-specific setup and configuration.**

- [Platform Overview](platform/) - Supported platforms
- [ESP32-S3 Guide](platform/esp32-s3.md) - Primary platform
- [Native Simulation](platform/native-sim.md) - Host PC testing
- [nRF54L15 Guide](platform/nrf54l15.md) - Nordic BLE
- [STM32 Guide](platform/stm32.md) - Experimental

---

### [Development](development/)
**Advanced development topics and workflows.**

- [Building WASM Apps](development/building-apps.md) - Complete workflow
- [SDK API Reference](development/sdk-api-reference.md) - Full function reference
- [Best Practices](development/best-practices.md) - Write efficient apps
- [SDK Troubleshooting](development/sdk-troubleshooting.md) - Debug app issues
- [OTA Updates](development/ota-updates.md) - Firmware deployment
- [Debugging](development/debugging.md) - Debug techniques

---

### [Hardware](hardware/)
**Custom hardware designs and schematics.**

- [AkiraConsole](hardware/index.md#akiraconsole) - Gaming console
- [AkiraMicro](hardware/index.md#akiramicro) - Wearable platform
- [Supported Boards](platform/) - Development kits

---

### [Resources](resources/)
**Additional learning materials and references.**

- [FAQ](resources/faq.md) - Frequently asked questions
- [Glossary](resources/glossary.md) - Technical terms
- [Performance](resources/performance.md) - Benchmarks

---

## 🎯 Use Cases

**IoT Gateways**
- Sensor data collection + cloud forwarding
- BLE mesh networking
- OTA updates for remote devices

**Wearables & Handhelds**
- Gaming consoles (AkiraConsole)
- Fitness trackers (AkiraMicro)
- Smartwatches with app stores

**Industrial Automation**
- Programmable logic with WASM
- Secure remote updates
- Multi-protocol connectivity

**Education & Research**
- WASM on embedded systems
- RTOS development
- Security sandboxing research

---

## 🔧 System Requirements

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| **RAM** | 256KB | 512KB + PSRAM |
| **Flash** | 2MB | 8MB+ |
| **CPU** | ARM Cortex-M3 | Dual-core @ 240MHz |
| **Connectivity** | None | WiFi + BLE |

---

## 🤝 Contributing

AkiraOS is open source and welcomes contributions!

- **Report Issues:** [GitHub Issues](https://github.com/akiraos/AkiraOS/issues)
- **Discussions:** [GitHub Discussions](https://github.com/akiraos/AkiraOS/discussions)
- **Contribute:** See [CONTRIBUTING.md](../CONTRIBUTING.md)

---

## 📄 License

AkiraOS is licensed under the [Apache License 2.0](../LICENSE).

---

## 🗺️ Project Roadmap

Current version: **v1.4.8 "GL1TCH"**

**Planned Improvements:**
- Transport interface for zero-copy networking
- Chunked WASM loading (50% memory reduction)
- Inline capability checks (40% faster)
- Per-app memory quotas
- AkiraMesh networking

See [IMPLEMENTATION_TASKS.md](../IMPLEMENTATION_TASKS.md) for detailed roadmap.

---

## 🔗 External Links

- **Zephyr Project:** [docs.zephyrproject.org](https://docs.zephyrproject.org)
- **WAMR:** [github.com/bytecodealliance/wasm-micro-runtime](https://github.com/bytecodealliance/wasm-micro-runtime)
- **WebAssembly:** [webassembly.org](https://webassembly.org)

---

<div align="center">

**Ready to get started?**

[Install AkiraOS →](getting-started/installation.md)

</div>



