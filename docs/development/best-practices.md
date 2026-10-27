---
layout: default
title: SDK Best Practices
parent: Development
nav_order: 2
---

# Akira SDK Best Practices

> **The canonical Best Practices guide lives in the [AkiraSDK submodule](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/BEST_PRACTICES.md).**
>
> To avoid duplicating content that evolves with the SDK, this guide is maintained in AkiraSDK. Refer to it directly — the local copy in the submodule is always in sync.

## Where to Find Best Practices

| Location | Path |
|----------|------|
| **Online (canonical)** | [github.com/ArturR0k3r/AkiraSDK — BEST_PRACTICES.md](https://github.com/ArturR0k3r/AkiraSDK/blob/v1.6.x/docs/BEST_PRACTICES.md) |
| **Local submodule** | `AkiraSDK/docs/BEST_PRACTICES.md` |

## Topics Covered

- **Main Loop** — How to structure `main()`, polling, yielding with `delay()`
- **Memory Management** — Static buffers, avoiding stack overflows, `mem_alloc()` checks
- **Display Optimization** — Dirty-region tracking, avoiding full redraws
- **GPIO & Input Polling** — Debouncing, edge detection patterns
- **Sensor Reading** — Rate limiting, averaging
- **Error Handling** — Defensive guards, graceful degradation
- **Power Efficiency** — Sleep patterns, wakeup sources
- **Code Organization** — File layout, naming conventions

## Related Documentation

- [SDK API Reference](sdk-api-reference.md) — Complete function reference
- [SDK Troubleshooting](sdk-troubleshooting.md) — Debug common app issues
- [Building WASM Apps](building-apps.md) — Build toolchain and workflow

---

*Last updated: 2026-09-14 (AkiraOS v1.6.5)*
