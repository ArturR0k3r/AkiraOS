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

> **Core execution figures now measured on hardware.** The
> [Capability Guard overhead](#capability-guard-overhead) (113 cycles ≈ 471 ns) and the
> [interpreter / AOT / native execution speeds](#webassembly-execution-speed) are measured
> on an ESP32-S3 at 240 MHz. Earlier revisions quoted unsourced figures — "~60 ns per native
> call" (the real guard is ~8× that), "AOT is 10–50× faster" and "AOT reaches ~90–95 % of
> native speed". The measured results supersede them: the classic interpreter is **71.8×
> slower than native C** and **78.3× slower than AOT**, and **AOT runs at native speed** (see
> below). Boot-time, throughput and power numbers remain unsourced and withdrawn until measured.

## Status of figures

| Figure | Compared against | Status | How it will be measured |
|--------|------------------|--------|-------------------------|
| [Capability Guard overhead](#capability-guard-overhead) | An identical unguarded native call | **113 cycles ≈ 471 ns** (ESP32-S3 @ 240 MHz, classic interpreter) | `bench/cg_microbench` — guarded vs unguarded native call, CCOUNT |
| [Interpreter vs native C](#webassembly-execution-speed) | The same compute kernel compiled natively for the same MCU | **71.8× slower** (ESP32-S3 @ 240 MHz) | `bench/exec_speed` — identical kernel, interpreter vs native, CCOUNT |
| [AOT speed-up](#webassembly-execution-speed) | The same kernel run by the WAMR **classic interpreter** on the same board | **78.3× faster than the interpreter** (ESP32-S3 @ 240 MHz) | `bench/exec_speed` — AOT vs interpreter, CCOUNT |
| [AOT vs native C](#webassembly-execution-speed) | The same kernel compiled natively for the same MCU | **≈ native speed** (0.92× native cycles; see caveat) | `bench/exec_speed` — AOT vs native, CCOUNT |
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

**Measured on ESP32-S3 @ 240 MHz.** The *same* compute kernel
([`bench/exec_speed/src/kernel.h`](https://github.com/ArturR0k3r/AkiraOS/blob/main/bench/exec_speed/src/kernel.h):
a register-only xorshift+LCG loop, 2 000 000 iterations) is run three ways and timed with
the Xtensa `CCOUNT` cycle counter. All three produce the **identical** result
(`0x3246764025`), a built-in check that they ran the same algorithm.

AkiraOS loads both `.wasm` and `.aot` files. A `.wasm` file runs on the WAMR **classic
interpreter** (the firmware does not enable WAMR's fast interpreter, see
[`zephyr/CMakeLists.txt`](https://github.com/ArturR0k3r/AkiraOS/blob/main/zephyr/CMakeLists.txt)).
An `.aot` file is machine code compiled ahead of time by `wamrc`, so no bytecode is decoded
at run time.

Result (AkiraConsole Prod, ESP32-S3 @ 240 MHz, 2 000 000 iterations, 3 runs, variance < 20
cycles out of billions):

| Execution mode | Total cycles | Time | Cycles/iter | Relative |
|----------------|--------------|------|-------------|----------|
| Native C (Xtensa, in firmware) | 36,008,200 | 150.0 ms | 18.0 | 1.0× (baseline) |
| **AOT** (`.aot`, `wamrc` LLVM) | 33,008,635 | 137.5 ms | 16.5 | **0.92× native** |
| Classic interpreter (`.wasm`) | 2,585,636,392 | 10,773 ms | 1292.8 | **71.8× native** |

- **Interpreter vs native C: 71.8× slower.** Each bytecode iteration costs ~1293 cycles
  versus 18 for native — the cost of decode-and-dispatch with no JIT.
- **AOT vs interpreter: 78.3× faster.** AOT removes the interpreter loop entirely.
- **AOT vs native C: ≈ native speed** (here 0.92×, i.e. ~8 % *fewer* cycles than native).
  **Caveat:** the native kernel is built by the firmware's Xtensa GCC at `-Os`
  (`CONFIG_SIZE_OPTIMIZATIONS=y`), while the AOT is LLVM `-O3` (`wamrc --opt-level=3`). On this
  tight integer loop LLVM `-O3` edges out GCC `-Os`; a speed-optimized native build would be
  expected to match or slightly beat AOT. The honest takeaway is that **AOT and native run in
  the same class**, far above the interpreter.

Methodology:

| | |
|--|--|
| Board | AkiraConsole Prod (`akiraconsole_prod`, ESP32-S3) |
| CPU clock | 240 MHz |
| Timer | Xtensa `CCOUNT`, benchmark thread pinned to core 1; only the compute loop is timed (host calls are outside it) |
| Kernel | One source (`kernel.h`) compiled natively (Xtensa GCC, in firmware) and to `wasm32` (`clang -O2`), the latter also AOT-compiled with `wamrc --target=xtensa --cpu=esp32s3 --opt-level=3`. Correctness is asserted by all three results matching bit-for-bit. |
| Source | [`bench/exec_speed`](https://github.com/ArturR0k3r/AkiraOS/tree/main/bench/exec_speed) — prints JSON harvested by `bench/akiraclaw_bench/scripts/collect.py` |

> **ESP32-S3 AOT cache-coherency fix.** Running AOT on the ESP32-S3 requires a fix to the
> WAMR Zephyr platform: `os_dcache_flush()` used `sys_cache_*`, which are no-ops on this SoC
> (no `CONFIG_CACHE` driver), so after the loader wrote `.aot` code through the PSRAM data-bus
> mirror the subsequent instruction-bus reads of section metadata returned stale cache
> (observed: `literal_size` read as garbage → corrupt code pointer → "target address out of
> range"). The bench patches `os_dcache_flush()` to use the ESP ROM primitives
> (`Cache_WriteBack_All` + `Cache_Invalidate_ICache_All`); this belongs upstream in the WAMR
> module so on-device AOT (including the retro cores) is reliable rather than
> cache-state-dependent. See
> [`bench/exec_speed/wamr_patch/zephyr_platform.c`](https://github.com/ArturR0k3r/AkiraOS/tree/main/bench/exec_speed/wamr_patch).

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

*Last updated: 2026-11-07 (AkiraOS v1.7.x) — interpreter/AOT/native execution speeds measured on ESP32-S3*
