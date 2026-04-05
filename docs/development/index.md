---
layout: default
title: Development
nav_order: 6
has_children: true
permalink: /development
---

# Development Guide

Advanced topics for AkiraOS WASM app development.

## SDK Documentation

### Application Development
- [SDK API Reference](sdk-api-reference.md) - Complete Akira SDK function reference
- [Best Practices](best-practices.md) - Patterns for efficient, maintainable code
- [SDK Troubleshooting](sdk-troubleshooting.md) - Debug common app issues

### Build & Deploy
- [Building WASM Apps](building-apps.md) - Complete WASM development workflow
- [Testing Apps with the Simulator](testing-apps.md) - Run WASM apps on your desktop before flashing
- [OTA Updates](ota-updates.md) - Over-the-air firmware deployment
- [Debugging](debugging.md) - Debug techniques and tools

### Reference
- [Build Options Reference](../reference/build-options.md) — Kconfig options for AkiraOS

### Contributing
See [CONTRIBUTING.md](../../CONTRIBUTING.md) for contribution guidelines.

## Quick Links

- **Build System:** West + CMake
- **Source Code:** `/src` directory
- **Tests:** `/tests` directory
- **Examples:** `AkiraSDK/wasm_apps/` directory (see [AkiraSDK](https://github.com/ArturR0k3r/AkiraSDK))

## Development Workflow

1. Write your app using the [SDK API](sdk-api-reference.md)
2. Follow [Best Practices](best-practices.md) for quality code
3. Build WASM binary (`AkiraSDK/wasm_apps/build.sh` or per-app `Makefile`)
4. Flash AkiraOS firmware to hardware (`west flash`)
5. Deploy WASM app via HTTP upload or shell
6. Debug with [Troubleshooting](sdk-troubleshooting.md) if needed

## Related Documentation

- [Architecture](../architecture) - System design
- [API Reference](../api-reference) - Low-level native APIs
- [Troubleshooting](../getting-started/troubleshooting.md) - Build and flash issues
