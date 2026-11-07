---
layout: default
title: Performance
parent: Resources
nav_order: 3
---

# Performance

This page is the single source for AkiraOS performance figures. Each figure is
either **measured**, with board, clock, method and benchmark source given, or marked
**not yet measured**. Other pages link here instead of repeating numbers.

> **No published measurements yet.** Earlier revisions of these docs quoted figures
> such as "~60 ns per native call", "AOT is 10–50× faster", "AOT reaches ~90–95 % of
> native speed", a boot time, throughput and power numbers. None of them came from a
> benchmark run. They have been withdrawn until
> [`bench/akiraclaw_bench`](https://github.com/ArturR0k3r/AkiraOS/tree/main/bench/akiraclaw_bench)
> publishes results.

## Status of figures

| Figure | Compared against | Status | How it will be measured |
|--------|------------------|--------|-------------------------|
| [Capability Guard overhead](#capability-guard-overhead) | The same native call with the guard compiled out | Not yet measured | `bench/akiraclaw_bench` B1: `wasm_claw` vs `wasm_noguard` |
| [AOT speed-up](#webassembly-execution-speed) | The same `.wasm` module run by the WAMR **classic interpreter** on the same board | Not yet measured | No benchmark yet (B1 builds with AOT off) |
| [AOT vs native C](#webassembly-execution-speed) | The same workload compiled natively for the same MCU | Not yet measured | No benchmark yet |
| WASM call path vs native call path | TFLite Micro inference called from C++ (`native_tflm`) | Not yet measured | `bench/akiraclaw_bench` B1: `native_tflm` vs `wasm_claw` |

## Capability Guard overhead

**Status: not yet measured.**

What runs on every guarded native call (allowed path):
`AKIRA_CHECK_CAP_OR_RETURN` → `akira_security_check_exec()`
([`src/runtime/security.c`](https://github.com/ArturR0k3r/AkiraOS/blob/main/src/runtime/security.c))
→ `wasm_runtime_get_module_inst()` → `wasm_runtime_get_custom_data()` → one 64-bit AND
against the app's `cap_mask`. These are ordinary function calls, not a single inlined
bit test. A **denied** call also writes a log line and an audit event. That path costs
much more, but only runs when a call is refused.

Methodology for the published figure:

| | |
|--|--|
| Board | AkiraConsole (`akiraconsole`, ESP32-S3) |
| CPU clock | 240 MHz |
| Timer | Xtensa `CCOUNT` cycle counter, benchmark thread pinned to core 1 |
| Builds | `prj.conf` (guard on) vs `prj_noguard.conf` (`CONFIG_AKIRA_CAPABILITY_SYSTEM=n`) |
| Source | [`bench/akiraclaw_bench`](https://github.com/ArturR0k3r/AkiraOS/tree/main/bench/akiraclaw_bench) B1, results harvested with `scripts/collect.py` |
| Reported as | Cycles per call, converted to ns at 240 MHz |

B1 currently times a whole `aiinfer_run()` inference at 1 µs resolution. That is too
coarse for a per-call difference below a microsecond, so the figure will come from a
tight loop over a guarded no-op native call, timed with `CCOUNT` in both builds.
<!-- TODO(metrics): add the guarded no-op loop to bench/akiraclaw_bench and publish
     cycles + ns here. Figures seen elsewhere (329 ns / 79 cycles, 0.8 µs) have no
     source in this repository. -->

## WebAssembly execution speed

**Status: not yet measured.**

AkiraOS loads both `.wasm` and `.aot` files. A `.wasm` file runs on the WAMR
**classic interpreter**: the firmware does not enable WAMR's fast interpreter, see
[`zephyr/CMakeLists.txt`](https://github.com/ArturR0k3r/AkiraOS/blob/main/zephyr/CMakeLists.txt).
An `.aot` file is machine code compiled ahead of time by `wamrc`, so no bytecode is
decoded at run time. Upstream WAMR describes AOT as
["near to native speed"](https://github.com/bytecodealliance/wasm-micro-runtime#key-features)
without a number. No AkiraOS benchmark yet measures either ratio:

- **AOT speed-up:** an `.aot` build compared with the same `.wasm` on the classic
  interpreter, on the same board.
- **AOT vs native C:** an `.aot` build compared with the same code compiled natively
  for the MCU.
<!-- TODO(metrics): add an interpreter-vs-AOT-vs-native compute benchmark (B1 builds
     with WAMR_BUILD_AOT=0 today) and publish both ratios with board and clock. -->

## Tools

- [`bench/akiraclaw_bench`](https://github.com/ArturR0k3r/AkiraOS/tree/main/bench/akiraclaw_bench):
  B1 WASM vs native and guard on/off, B2 MPU sandbox cost, B3 audit log, B4 memory,
  B5 BLE. `scripts/collect.py` harvests the JSON results from the serial console.
- Shell: `kernel threads`, `kernel stacks` and `kernel heap` for stack and heap
  headroom. The `benchmark` shell command times button reads, stats queries and
  `memset`; it does not measure native-call or WASM-load cost.

## Related Documentation

- [Architecture Overview](../architecture)
- [Runtime Architecture](../architecture/runtime.md)
- [AOT Compilation](../architecture/aot-compilation.md)

---

*Last updated: 2026-11-07 (AkiraOS v1.6.6)*
