---
layout: default
title: SDK Troubleshooting
parent: Development
nav_order: 5
---

# Akira SDK Troubleshooting

> **The canonical Troubleshooting guide lives in the [AkiraSDK submodule](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/TROUBLESHOOTING.md).**
>
> To avoid duplicating content that evolves with the SDK, this guide is maintained in AkiraSDK. Refer to it directly — the local copy in the submodule is always in sync.

## Where to Find Troubleshooting Guides

| Location | Path |
|----------|------|
| **Online (canonical)** | [github.com/ArturR0k3r/AkiraSDK — TROUBLESHOOTING.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/TROUBLESHOOTING.md) |
| **Local submodule** | `AkiraSDK/docs/TROUBLESHOOTING.md` |

## Topics Covered

- **Display Issues** — Blank screen, wrong colors, flickering
- **GPIO Problems** — Reads returning incorrect values, output not toggling
- **Sensor Issues** — No data, sampling rate, calibration
- **RF Communication** — Init failures, send/receive errors
- **Timer Issues** — Incorrect elapsed time, timer drift
- **Storage Problems** — File not found, write failures, quota exceeded
- **BLE Issues** — Connection failures, characteristic write errors
- **HID Problems** — Keyboard not recognized, mouse drift
- **Network Issues** — Connection timeouts, socket errors
- **Memory / Crash** — Stack overflows, heap exhaustion, OOB traps
- **Build Errors** — Compiler, linker, manifest embedding errors

## AkiraOS-Level Troubleshooting

For issues related to the AkiraOS firmware itself (build failures, flash errors, boot problems), see:

- [Getting Started Troubleshooting](../getting-started/troubleshooting.md)
- [Debugging Guide](debugging.md)

## Related Documentation

- [SDK API Reference](sdk-api-reference.md) — Complete function reference
- [Best Practices](best-practices.md) — Patterns for reliable apps
- [Building WASM Apps](building-apps.md) — Build toolchain and workflow

---

*Last updated: 2026-09-14 (AkiraOS v1.6.4)*
