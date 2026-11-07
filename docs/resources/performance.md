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

> **Few measurements published so far.** The [Capability Guard overhead](#capability-guard-overhead)
> is now measured on hardware (113 cycles ≈ 471 ns, ESP32-S3 @ 240 MHz). Earlier revisions
> quoted unsourced figures — "~60 ns per native call" (the real guard is ~8× that),
> "AOT is 10–50× faster", "AOT reaches ~90–95 % of native speed", plus boot-time,
> throughput and power numbers — none from a benchmark run. Those remain withdrawn until
> measured.

## Status of figures

| Figure | Compared against | Status | How it will be measured |
|--------|------------------|--------|-------------------------|
| [Capability Guard overhead](#capability-guard-overhead) | An identical unguarded native call | **113 cycles ≈ 471 ns** (ESP32-S3 @ 240 MHz, classic interpreter) | `bench/cg_microbench` — guarded vs unguarded native call, CCOUNT |
| [AOT speed-up](#webassembly-execution-speed) | The same `.wasm` module run by the WAMR **classic interpreter** on the same board | Not yet measured | No benchmark yet (B1 builds with AOT off) |
| [AOT vs native C](#webassembly-execution-speed) | The same workload compiled natively for the same MCU | Not yet measured | No benchmark yet |
| WASM call path vs native call path | TFLite Micro inference called from C++ (`native_tflm`) | Not yet measured | `bench/akiraclaw_bench` B1: `native_tflm` vs `wasm_claw` |

## Capability Guard overhead

**Measured: ~113 CPU cycles ≈ 471 ns per guarded native call** on ESP32-S3 at 240 MHz
with the WAMR classic interpreter.

What runs on every guarded native call (allowed path):
`AKIRA_CHECK_CAP_OR_RETURN` → `akira_security_check_exec()`
([`src/runtime/security.c`](https://github.com/ArturR0k3r/AkiraOS/blob/main/src/runtime/security.c))
→ `wasm_runtime_get_module_inst()` → `wasm_runtime_get_custom_data()` → one 64-bit AND
against the app's `cap_mask`. These are ordinary function calls, not a single inlined
bit test — which is why the real cost is well above the ~60 ns an inlined check would
imply. A **denied** call also writes a log line and an audit event; that path costs more
but only runs when a call is refused.

Result (AkiraConsole Prod, ESP32-S3 @ 240 MHz, 200 000 iterations/run, 4 runs, **zero
variance**):

| Native call | Cycles/call | Time/call |
|-------------|-------------|-----------|
| Unguarded (baseline) | 903.2 | ~3.76 µs |
| Guarded (capability granted, allow path) | 1016.3 | ~4.23 µs |
| **Capability Guard overhead** | **113.0** | **≈ 471 ns** |

The unguarded baseline (~903 cycles) is the WAMR **classic interpreter's** per-native-call
dispatch cost, not the guard; the guard is the 113-cycle delta on top. Under AOT both the
dispatch and the delta would differ — this figure is interpreter-measured.

Methodology:

| | |
|--|--|
| Board | AkiraConsole Prod (`akiraconsole_prod`, ESP32-S3) |
| CPU clock | 240 MHz |
| Timer | Xtensa `CCOUNT` cycle counter, benchmark thread pinned to core 1 |
| Method | A WASM module calls an identical pair of natives N times — one guarded by `AKIRA_CHECK_CAP_OR_RETURN` (capability granted so the check takes its allow path), one unguarded. The shared loop and native-dispatch cost cancel in the difference. |
| Source | [`bench/cg_microbench`](https://github.com/ArturR0k3r/AkiraOS/tree/main/bench/cg_microbench) — links the real `src/runtime/security.c`; prints JSON harvested by `bench/akiraclaw_bench/scripts/collect.py` |
| Reported as | Cycles per call, converted to ns at 240 MHz |

Figures seen elsewhere (329 ns / 79 cycles, 0.8 µs) have no source in this repository and
do not match this measurement.

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

*Last updated: 2026-11-07 (AkiraOS v1.6.7)*
