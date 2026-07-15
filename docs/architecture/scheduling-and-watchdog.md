# Cooperative Scheduling Model & Watchdog Contract

_Architecture document · AkiraOS v1.5.4_

---

## Overview

AkiraOS hosts WASM applications on top of a **Zephyr RTOS** kernel.  Because
the WAMR interpreter runs each WASM module inside a dedicated Zephyr thread,
a poorly-written app can monopolise the CPU.  AkiraOS addresses this through
two complementary mechanisms:

1. **Cooperative scheduling yield points** — WASM apps must yield at regular
   intervals so that the OS can service other threads.
2. **Two-tier watchdog** — a software execution watchdog kills stuck WASM
   functions; a hardware WDT resets the whole device if the OS itself hangs.

---

## Thread Model

```
  ┌─────────────────────────────────────────────┐
  │  Zephyr kernel (preemptive, tick-based)      │
  │                                              │
  │  ┌──────────────┐  ┌──────────────────────┐  │
  │  │  main thread │  │  WASM app thread(s)  │  │
  │  │  (prio 0 K_  │  │  (prio 5 K_PRIO_     │  │
  │  │   PRIO_COOP) │  │   PREEMPT, per app)  │  │
  │  └──────────────┘  └──────────┬───────────┘  │
  │                               │              │
  │  ┌───────────────────────────┐│              │
  │  │  WDT auto-feed worker     ││              │
  │  │  (K_PRIO_COOP, periodic)  ││              │
  │  └───────────────────────────┘│              │
  │                               │              │
  │  ┌────────────────────────────▼────────────┐ │
  │  │  sandbox_check_syscall() hot path        │ │
  │  │  • capability bitmask check (< 100 ns)  │ │
  │  │  • rate limit counter                   │ │
  │  │  • exec-time accumulator for soft-WDT   │ │
  │  └──────────────────────────────────────── ┘ │
  └─────────────────────────────────────────────┘
```

Each WASM app runs in its own Zephyr thread at a **preemptible** priority,
so the kernel time-slices between apps automatically.  The main thread and
WDT worker run at cooperative priority and are never preempted by WASM threads.

---

## Cooperative Yield Contract

### Why cooperative yield points exist

Although Zephyr's scheduler is preemptive, the WAMR interpreter executes a
WASM function call atomically from the host's perspective — there is no
kernel preemption *inside* a single native→WASM call.  Long-running WASM
functions (tight loops, large memory copies) therefore block their thread
until the call returns.

To guarantee bounded latency for other threads, AkiraOS requires every WASM
app to **yield at least once per `CONFIG_AKIRA_SANDBOX_EXEC_TIMEOUT_MS`**
(default: 5 000 ms).  Failing to do so is a contract violation that triggers
the software execution watchdog (see below).

### How to yield from a WASM app

From a WASM app written in C (via the AkiraSDK):

```c
#include "akira_sdk.h"

void my_heavy_computation(void) {
    for (int i = 0; i < 1000000; i++) {
        /* ... work ... */
        if ((i % 10000) == 0) {
            akira_yield();   /* cooperative yield — mandatory every ~50 ms */
        }
    }
}
```

`akira_yield()` maps to `akira_sleep_ms(0)` on the host side, which calls
`k_yield()` in the WASM thread, allowing the scheduler to run other threads.

### Yield budget

| Context | Maximum time without yield |
|---------|---------------------------|
| Normal WASM app | `CONFIG_AKIRA_SANDBOX_EXEC_TIMEOUT_MS` (default 5 s) |
| Startup (`_start`) | `CONFIG_AKIRA_WASM_START_TIMEOUT_MS` (default 15 s) |
| Shutdown (`_exit`) | `CONFIG_AKIRA_WASM_STOP_TIMEOUT_MS` (default 5 s) |

When invoked, `sandbox_watchdog_kill()`:
1. Sets `ctx.exec_active = false` and increments `ctx.watchdog_kills`.
2. Calls `wasm_runtime_terminate()` on the WASM module instance.
3. Records an audit log entry (if `CONFIG_AKIRA_SECURITY_AUDIT=y`).
4. The app manager marks the app as `APP_STATE_FAILED` and applies the
   auto-restart policy (`CONFIG_AKIRA_APP_MAX_RETRIES`).

> **Status — enforcement not yet wired.** The kill path above exists and is
> unit-tested (`tests/src/test_security.c`), but no periodic checker in the
> runtime currently measures a running call against its yield budget and
> invokes it. The yield budget is therefore **advisory** today:
> `sandbox_exec_begin/end` bracket a call for telemetry, but a tight loop
> that never yields is not force-killed. Wiring a periodic exec-timeout
> check (or a Zephyr thread/hardware watchdog) is a TODO.

---

## Software Execution Watchdog

The software watchdog is part of the **sandbox** layer
(`src/runtime/security/sandbox.h`, `@stability stable @since 1.3`).

### Lifecycle

```
sandbox_exec_begin(&ctx)
    │
    │  WASM function executes ...
    │
    │  (PLANNED, not yet wired: a periodic checker would compare
    │   elapsed vs exec_timeout_ms here and call sandbox_watchdog_kill();
    │   see the status note above)
    │
sandbox_exec_end(&ctx)
```

### Key fields in `sandbox_ctx_t`

| Field | Type | Meaning |
|-------|------|---------|
| `exec_active` | `bool` | True while a WASM call is in flight |
| `exec_timeout_ms` | `uint32_t` | Per-app timeout; default `AKIRA_SANDBOX_EXEC_TIMEOUT_MS` |
| `exec_start_uptime_ms` | `uint64_t` | `k_uptime_get()` at `exec_begin` |
| `watchdog_kills` | `uint32_t` | Cumulative kill count; visible in `akira stats` |
| `denied_syscalls` | `uint32_t` | Cumulative denied calls |

### Configuring per-app timeout

Set `"exec_timeout_ms"` in the app manifest:

```json
{
  "name": "my_heavy_app",
  "capabilities": ["timer", "storage.read"],
  "exec_timeout_ms": 10000
}
```

Values are clamped to `[1000, 30000]` ms.

---

## Hardware Watchdog Contract

The hardware WDT is a **system-level liveness guarantee** independent of any
WASM app.  It detects OS-level hangs (deadlocked threads, runaway ISRs) that
the software watchdog cannot catch.

### Configuration (`CONFIG_AKIRA_WDT=y`)

```
CONFIG_WATCHDOG=y             # Zephyr WDT subsystem
CONFIG_AKIRA_WDT=y            # AkiraOS WDT integration
CONFIG_AKIRA_WDT_TIMEOUT_MS=30000    # reset if not fed within 30 s
CONFIG_AKIRA_WDT_FEED_INTERVAL_MS=10000   # auto-feed every 10 s
```

**Invariant:** `FEED_INTERVAL_MS < TIMEOUT_MS`.  The build will fail if this
is violated (checked via Kconfig `range` and a static assert in
`src/drivers/wdt/akira_wdt.c`).

### Auto-feed worker

A dedicated cooperative-priority Zephyr thread calls `akira_wdt_feed()`
every `CONFIG_AKIRA_WDT_FEED_INTERVAL_MS`.  Because it runs at cooperative
priority, it only executes when no higher-priority preemptible thread is
running.  This means:

> **If a WASM app thread starves the scheduler for longer than
> `AKIRA_WDT_TIMEOUT_MS`, the hardware resets the device.**

The feed worker is intentionally designed to *not* be preemptible by WASM
threads, ensuring the hardware WDT fires if the entire OS is live but the
scheduler is stuck.

### WASM app opt-in petting

When `CONFIG_AKIRA_WASM_WDT=y`, a WASM app with the `"wdt"` capability can
call `wdt_pet()` to reset the hardware WDT timeout window.  This is intended
for long-running, latency-sensitive apps (e.g., a hardware control loop)
that need more control over the WDT window.

```c
/* In WASM app — requires "wdt" capability in manifest */
wdt_pet();   /* Resets the hardware WDT countdown */
```

**Security note:** `wdt_pet()` requires the `AKIRA_CAP_WDT` capability.
Granting this to an untrusted app allows it to prevent automatic device
recovery.  Only grant `"wdt"` to system-level apps.

---

## Interaction Between the Two Watchdogs

```
  WASM function starts
       │
       ├─ Software WDT timer tick  ──▶  > exec_timeout_ms?
       │                                      │
       │                                      └─ YES → sandbox_watchdog_kill()
       │                                               (app terminated, OS lives)
       │
       └─ Hardware WDT auto-feed  ──▶  WDT reset counter
          (every 10 s)                 (starvation guard)

  If OS hangs (no auto-feed):
       └─ Hardware WDT fires → full device reset after TIMEOUT_MS
```

The two watchdogs are **independent** and guard against different failure
modes:

| Failure | Caught by |
|---------|-----------|
| Single WASM function runs too long | Software WDT → app kill |
| WASM app monopolises CPU for > TIMEOUT_MS | Hardware WDT → device reset |
| OS deadlock / kernel hang | Hardware WDT → device reset |
| WASM app crash / exception | WAMR exception handler → app kill |

---

## Scheduling Priorities Reference

| Thread | Priority | Preemptible? |
|--------|----------|--------------|
| WDT auto-feed worker | `K_PRIO_COOP(1)` | No |
| Main / boot thread | `K_PRIO_COOP(0)` | No |
| App manager | `K_PRIO_PREEMPT(2)` | Yes |
| WASM app threads | `K_PRIO_PREEMPT(5)` | Yes |
| BLE/Net stack | `K_PRIO_PREEMPT(3)` | Yes |
| Shell thread | `K_PRIO_PREEMPT(7)` | Yes |

Lower number = higher priority.  Cooperative threads (`COOP`) never yield
to preemptible threads; they only switch when explicitly calling `k_yield()`
or a blocking primitive.

---

## Board-Specific Notes

### `native_sim`

The `native_sim` board does not have a hardware WDT device; `CONFIG_AKIRA_WDT`
is disabled.  The software execution watchdog is active and tested by the
`tests/src/test_security.c` suite (see `test_sandbox_watchdog_kill`).

### ESP32 / ESP32-S3

The ESP32 Timer Group 0 WDOG is used.  The default timeout of 30 s is
appropriate; reduce to 5–10 s for safety-critical deployments.

### STM32 / STM32U5 (STEVAL-STWINBX1)

The IWDG (independent watchdog) is used.  Maximum IWDG timeout on STM32U5
is approximately 32 s; keep `CONFIG_AKIRA_WDT_TIMEOUT_MS` within this range.

---

## FAQ

**Q: My WASM app does heavy computation. How do I avoid being killed?**  
A: Call `akira_yield()` at least once per 5 seconds.  Alternatively, set
`"exec_timeout_ms": 15000` in your app manifest (max 30 000 ms).

**Q: The hardware WDT fired and reset my device. What happened?**  
A: The `watchdog_kills` counter in the Akira shell (`akira stats`) will be
non-zero if the software WDT caught the problem first.  If `watchdog_kills`
is 0, the OS itself hung (kernel deadlock or ISR starvation).  Enable
`CONFIG_DEBUG_COREDUMP` to capture a post-mortem trace.

**Q: Can I disable the watchdogs for debugging?**  
A: Set `CONFIG_AKIRA_WDT=n` to disable the hardware WDT.  Set
`CONFIG_AKIRA_SANDBOX_EXEC_TIMEOUT_MS=30000` to extend the software WDT to
its maximum.  Never disable both in production firmware.

---

_See also:_  
- [`docs/api-stability-policy.md`](../api-stability-policy.md) — API stability rules  
- [`src/runtime/security/sandbox.h`](../../src/runtime/security/sandbox.h) — sandbox API  
- [`src/drivers/wdt/akira_wdt.h`](../../src/drivers/wdt/akira_wdt.h) — WDT API  
- [Zephyr Scheduling docs](https://docs.zephyrproject.org/latest/kernel/scheduling/index.html)
