---
layout: default
title: Getting Started
nav_order: 2
has_children: true
permalink: /getting-started
---

# Getting Started with AkiraOS

**From zero to running WASM apps in under 30 minutes.**

## What You'll Learn

- Install development environment
- Build and flash AkiraOS firmware
- Deploy your first WASM application
- Troubleshoot common issues

## Quick Navigation

1. [Installation Guide](installation.md) - Set up dev environment
2. [Building Your First App](first-app.md) - Hello World in WASM
3. [Troubleshooting](troubleshooting.md) - Common issues and fixes

## Prerequisites

| Requirement | Version | Purpose |
|-------------|---------|---------|
| Linux/WSL2 | Ubuntu 20.04+ | Build environment |
| Python | 3.8+ | West tool, scripts |
| Git | 2.25+ | Source control |
| ~5GB disk space | - | SDK + dependencies |

## Development Workflow

```mermaid
graph LR
    A[Setup Environment] --> B[Clone Repository]
    B --> C[Build Firmware]
    C --> D[Flash Hardware]
    D --> E[Build WASM App]
    E --> F[Upload via HTTP]
    F --> G[App Running!]
```

## Platform Support

| Platform | Status | Flash Method | Notes |
|----------|--------|--------------|-------|
| ESP32-S3 | ✅ Fully supported | UART/USB | Primary target, WiFi + BT |
| Native Sim | ✅ Fully supported | N/A | Testing only |
| nRF54L15 | ✅ Supported | J-Link/SWD | BLE + sensors |
| STM32 | ⚠️ Experimental | SWD | Limited testing |

[Detailed platform guides →](../platform/)

## Typical Timeline

- **Environment Setup:** 20-30 minutes (one-time)
- **First Firmware Build:** 5-10 minutes
- **Flash to Hardware:** 1-2 minutes
- **Build WASM App:** 1-2 minutes
- **Upload & Run:** 30 seconds

## Next Steps

1. **New Users:** Start with [Installation Guide](installation.md)
2. **Hardware Ready:** Jump to [First App Tutorial](first-app.md)
3. **Experienced Users:** Check [API Reference](../api-reference/)

## Getting Help

- **Documentation:** Browse the full [architecture docs](../architecture/)
- **Issues:** Check [troubleshooting guide](troubleshooting.md)
- **Community:** GitHub Issues & Discussions

## Related Documentation

- [System Architecture](../architecture/) - Deep dive into design
- [API Reference](../api-reference/) - WASM application APIs
- [Platform Guides](../platform/) - Board-specific setup
- [Development Guide](../development/) - Advanced topics
