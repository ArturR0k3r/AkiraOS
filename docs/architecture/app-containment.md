---
layout: default
title: App Containment
parent: Architecture
nav_order: 10
---

# App containment

A WASM app must not be able to crash or hang the system, whatever it does:
bad pointers, endless loops, crashes inside a native call, waits that never
end, memory it never frees, resources it abandons, or hardware the OS itself
uses. A misbehaving app can only hurt itself. This page describes how AkiraOS
enforces that and where the guarantee ends.

An app's manifest declares the capabilities it *wants*; it doesn't prove what
the app is *allowed*. So anything that could hurt the system is also blocked
by fixed rules (allowlists, clamps, quotas) that don't depend on the manifest.

## Scheduling

App threads run at `CONFIG_AKIRA_WASM_APP_PRIORITY` (default 14, the lowest
preemptible priority, shared with the shell) with 20 ms time slicing. An app
that never yields gets its share of the CPU and no more: every system thread
(drivers, network, Bluetooth, the watchdog feeder, the system workqueue) runs
at a higher priority and preempts it.

## Stopping an app

`akira_runtime_stop()` (`app stop`, an app switch, `app_stop()` from another
app) works in stages:

1. **Ask.** The runtime sets the app's stop flag, runs every `on_stop` hook
   (see [Writing natives](#writing-natives)), wakes the app's stop-aware
   waits and terminates the WAMR instance.
2. **Grace (1.5 s).** A cooperative app is gone within a frame.
3. **Abort.** A thread still on the CPU after the grace period is spinning in
   WASM code (WAMR checks for termination only at calls), so it holds no
   system lock and is aborted. A thread blocked in a native call gets up to
   `CONFIG_AKIRA_WASM_STOP_TIMEOUT_MS` (5 s).

   A thread inside a **critical section** is never aborted. A native that
   holds a bus, driver, filesystem or flash lock does so only inside a span
   marked with `akira_native_critical_enter/exit()`. The stop path waits up
   to 3× the timeout for the thread to leave the span; if it hasn't, the stop
   returns `-EBUSY` and the app ends when the native returns. Every such span
   is bounded (file I/O runs in 4 KB steps, for example). The thread is
   suspended while the stop path checks, so it can't enter a span between
   the check and the abort.

An aborted app's slot is reclaimed: its resources are released, its instance,
module and binary are freed, and the slot is free for the next app.

## Crashes

Every `k_sys_fatal_error_handler` in the tree (the default one in
`app_resources.c`, `AKIRA_PANIC`'s and the boot guard's) asks
`akira_runtime_contain_fault()` first. If the faulting thread is a WASM app
thread, the fault happened in thread context, and it isn't a kernel panic,
then:

- only that thread is aborted;
- the slot is reclaimed on the system workqueue;
- the app manager marks the app crashed (`AKIRA_HOOK_APP_CRASHED`) and applies
  its restart policy.

This covers a fault inside a native call made by the app. If the native was
inside a critical section, the thread died holding a system lock: the release
hooks could block on it forever, so they don't run, and the slot keeps its
memory and resources and stays **parked** (never reused) until reboot. The
system keeps running; that lock's driver may stay busy.

A fault anywhere else (an ISR, a system thread, a kernel panic) keeps
Zephyr's policy, and so does a stack overflow on a board without a hardware
stack guard (`CONFIG_HW_STACK_PROTECTION`): the sentinel or a canary finds it
only after it may have overwritten heap memory next to the stack, if at all.
On AkiraConsole Prod (ESP32-S3) a runaway native recursion is not detected:
the device hangs until the hardware watchdog resets it (about 30 s,
`CONFIG_AKIRA_WDT_TIMEOUT_MS`). WAMR keeps
`CONFIG_AKIRA_WASM_STACK_GUARD_SIZE` free for natives, so only a native that
uses more than that can overflow: a firmware bug, not something an app
controls. A contained app fault is not a reason for
the boot guard to roll back firmware.

## Memory

- **Linear memory.** WAMR validates every module (`CONFIG_AKIRA_WAMR_MINI_LOADER`
  is off by default) and bounds-checks every memory access.
- **Native stack.** WAMR keeps `CONFIG_AKIRA_WASM_STACK_GUARD_SIZE` (3 KB) of
  the app thread's stack free, so deep recursion traps the app with room left
  for the native call it was making.
- **Heap.** Everything WAMR allocates while the app runs (its instance, linear
  memory, WASM stack, each `memory.grow`) is charged to the app and capped at
  `CONFIG_AKIRA_WASM_APP_MEM_LIMIT_KB` (2 MB with PSRAM, 1 MB without). On
  boards with PSRAM it never falls back to internal SRAM, which drivers,
  Bluetooth and Wi-Fi need. Without PSRAM, an allocation an app controls is
  refused if it would leave less than `CONFIG_AKIRA_WASM_APP_HEAP_RESERVE_KB`
  (32 KB) of the system heap free. Either way, the app's own allocation fails,
  not the system's.
- **Pointers.** Natives check every pointer and its full length against the
  app's linear memory before taking any lock.

## Cleanup on exit

An app can end four ways: `main()` returns, it traps, it is stopped, or its
thread is aborted. Only the first two run the app's own exit path. So
everything an app can leave behind is owned by its slot and released by
hooks registered with `AKIRA_APP_RESOURCE_DEFINE()`. These run on every exit
path, before the app's memory is freed:

| Area (order) | Released |
|---|---|
| Network (10) | TCP/UDP streams closed, rings in app memory detached, queued events purged. First, because the poll thread writes into app memory |
| Display (20) | pending auto-flush cancelled |
| Mesh (40) | the session, only if this app owns it |
| BLE (50) | the app's advert stopped, its GATT services removed, BLE mode, device name and bonding restored |
| MQTT (50) | subscriptions dropped, HA light records removed, publish budget reset |
| HID (50) | keys, buttons, mouse, gamepad and consumer state released, the app's actions dropped, transport settings restored |
| OTA (50) | a half-written update aborted, a leaked socket closed |
| Files (50) | every fs/storage descriptor closed |
| Timers, RTC alarms, IPC (50) | handles freed, alarms cleared, subscriptions and queues freed |
| GPIO, PWM, UART (50) | pins the app configured disconnected, PWM outputs stopped, UART ports closed and their line settings restored |
| Matter, AI inference (50) | endpoint ownership dropped, model handles freed |
| Lifecycle, app output (50) | pending `app_start`/`app_stop` results dropped, output budget reset |
| RF (60) | TX/RX stopped and the radio released, if the app used it |
| Host allocations (90) | `akira_wasm_malloc` blocks freed |

Each hook touches only what that slot owns, so it is safe whatever the app
left behind, and safe to run twice.

## Waits

No native waits for an app-chosen time without checking for a stop.
`delay()`, `sleep_ms()`, `msg_recv()`, `rf_recv_pop()`, `mesh_recv_pop()`,
`mqtt_poll()`, `matter_poll()`, network connects and app-to-app requests run in
slices or stop-aware sleeps and end as soon as the app is stopped: `delay()`
returns early (still 0), the others return an error (`-EINTR` /
`-ECANCELED`). RF and mesh receive timeouts and lifecycle requests are capped
at 60 s; `msg_recv()`, `mqtt_poll()` and `matter_poll()` with timeout -1 wait
until a message arrives or the app is stopped.

`app_start()`, `app_stop()` and `app_install_from_sd()` run on the lifecycle
work queue; the calling app waits for the result stop-aware. An app can't
start itself, reinstall itself, or issue more than a burst of 4 such requests
and then one per 2 s.

## Hardware and shared resources

| Resource | Rule |
|---|---|
| GPIO | Chip-reserved pins (ESP32-S3 flash/PSRAM, USB, UART0) are refused to everyone. Apps also get only `CONFIG_AKIRA_WASM_GPIO_APP_PINS`, minus every pin the devicetree gives the system and the strapping pins (ESP32-S3: GPIO45/46). AkiraConsole Prod's allowlist is GPIO39 and GPIO40. An app reads and writes only pins it configured. |
| ADC | `CONFIG_AKIRA_WASM_ADC_CHANNEL_MASK`; channels whose pad is a system pin are refused. AkiraConsole Prod uses SAR ADC1 channel 1 (GPIO2, the RV1 pot). |
| I2C | Addresses of the bus's devicetree children, the reserved ranges and, on AkiraConsole Prod, the LP5817, SE050 and STC3115 are refused. Transfers are capped at 256 bytes. |
| UART | System UARTs (console, shell, Bluetooth, mcumgr, any with a device node) are refused. A write sends at most 4096 bytes or ~250 ms per call. |
| PWM | Only devicetree LEDC channels that no system node (or shared timer) uses, on an allowed pad. |
| Radio | Raw capture and replay are clamped to 2 s per call. Parameters are range-checked in the driver. |
| Files | At most 2 open files per app and 2 for all apps together, leaving the OS two slots. Writes past `CONFIG_AKIRA_WASM_{FS,STORAGE}_QUOTA_BYTES` per file, or that would leave less than a quarter of the volume free, return `-ENOSPC`. Paths are jailed to the app's directory. I/O runs in 4 KB steps. |
| Settings | Each app has a private `a/<ns>/` namespace bound to its exact name: 40 keys per app, 64 for all apps, rate-limited writes. OS keys (Wi-Fi, MQTT, mesh keys, boot guard) are out of reach. |
| Memory | IPC queues, timers, app allocations and AI models come from PSRAM (or the app's own heap) on boards that have it, never internal DRAM. |
| Watchdog | `wdt_pet()` never feeds the hardware watchdog. |
| Power | Apps can't select light sleep, deep sleep or hibernate. |
| OTA | Apps can't confirm an image; rollback is refused while an update is in progress; one app OTA operation at a time. |
| Network | Streams belong to their app; one connect at a time system-wide; sends never block; ring binds need linear memory that can't grow. Egress policy (`akira_net_policy.h`) still applies. |
| BLE, HID, mesh, Matter | One owner at a time where the state is shared; other apps get `-EBUSY` or `-EPERM`. |

## Flooding

App output (`printf`, `akira_log`, WASI stdout) is limited per app to a 2 KB
burst then 1 KB/s, 160 characters per line. Capability-denial warnings are
limited to one per second per app, and other app-triggered warnings are
rate-limited or logged at debug level. MQTT publishes, BLE advertising and
GATT changes, device renames and power-control calls have their own rate
limits.

On AkiraConsole Prod logging is deferred: an app thread only queues a log
line, so a thread aborted while logging holds no console lock.

## Writing natives

Product firmware that adds natives (`AKIRA_NATIVE_API_DEFINE()`) must follow
the same rules, using the helpers in `include/akira_app_containment.h`:

| Rule | Helper |
|---|---|
| Validate every pointer *and its full length* before taking a lock. WAMR's `*` signature checks one byte; `*~` checks the length that follows. | `akira_wasm_ptr_to_native()`, `wasm_runtime_validate_native_addr()` |
| Never wait forever, and never `k_sleep()` for an app-chosen time. | `akira_runtime_sleep()`, `akira_runtime_stop_requested()` |
| Hold a system lock only inside a critical span, take it with a bounded wait, and keep the span bounded. | `akira_native_critical_enter/exit()` |
| Record anything that outlives the call with the caller's slot and release it when the app ends. | `akira_runtime_slot_of_exec_env()`, `AKIRA_APP_RESOURCE_DEFINE()` |
| Never keep a pointer into app memory after the call returns; copy into host buffers. | |
| Never stop or join the calling app synchronously from its own thread. | queue it |

```c
static void valve_release(int slot, const char *app_name)
{
    if (g_valve_owner == slot) {
        acme_valve_close();
        g_valve_owner = -1;
    }
}
AKIRA_APP_RESOURCE_DEFINE(acme_valve, 50, NULL, valve_release);
```

`order` (here 50) sets the release order, lowest first: 10 for subsystems that
write into app memory from other threads, 50 by default.

## Limits

- **AOT is native code.** wamrc compiles bounds checks into an `.aot` file,
  but a hand-crafted one can do anything, including everything this page
  rules out. Every build accepts unattested `.aot` files (with a warning and
  an audit record): installing one is the user's choice and risk. Install
  only files you built with wamrc yourself or that are signed, or build with
  `CONFIG_AKIRA_REFUSE_UNATTESTED_AOT=y` to run AOT only for attested apps.
- **A lock held when an app faults inside a critical section** is not
  recovered. The system keeps running, but that driver may stay busy and the
  app's slot stays parked until reboot.
- **native_sim** can't preempt a WASM busy loop, so busy-loop tests only run
  on hardware.

## Testing

`CONFIG_AKIRA_SANDBOX_SELFTEST=y` (refused in release builds) exports
`selftest_fault(kind)`, which faults on purpose in the app's own thread:

| kind | Fault |
|---|---|
| 0 | `k_oops()` |
| 1 | NULL write (hardware only; on native_sim it kills the host process) |
| 2 | runaway native recursion (hardware only). Contained only with a hardware stack guard; without one (ESP32-S3) the device hangs until the hardware watchdog resets it |
| 3 | spin forever inside the native, holding no lock |
| 4 | `k_oops()` inside a critical section: the slot must stay parked |

The log must show "App '…' crashed … stopped it, system unaffected" and the
system must stay usable.

Sources: [`akira_app_containment.h`](https://github.com/ArturR0k3r/AkiraOS/blob/main/include/akira_app_containment.h),
[`akira_runtime.c`](https://github.com/ArturR0k3r/AkiraOS/blob/main/src/runtime/akira_runtime.c),
[`app_resources.c`](https://github.com/ArturR0k3r/AkiraOS/blob/main/src/runtime/app_resources.c),
[`sandbox_selftest.c`](https://github.com/ArturR0k3r/AkiraOS/blob/main/src/runtime/sandbox_selftest.c).
