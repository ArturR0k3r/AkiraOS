---
layout: default
title: OTA Design
parent: Architecture
nav_order: 10
---

# AkiraOS OTA Update System — Design Document

**Version:** 1.6.5  
**Status:** Approved for implementation  
**Scope:** Track A — OTA with atomic rollback and delta updates

---

## 1. Goals

| # | Goal |
|---|------|
| G1 | Atomic slot-swap: only boot new firmware after explicit confirmation |
| G2 | Automatic rollback when a new image crashes, hangs or never becomes healthy |
| G3 | Delta (binary-patch) update mode to reduce download size |
| G4 | WASM app can trigger / monitor OTA without elevated native access |
| G5 | Single `docs/feature_overhead.md` entry tracking flash/RAM cost |

---

## 2. Current State (v1.6.5)

`src/connectivity/ota/ota_manager.c/h` already provides:
- MCUboot `boot_request_upgrade()` / `boot_write_img_confirmed()` wrappers
- `ota_start_update()`, `ota_write_chunk()`, `ota_finalize_update()`
- `ota_confirm_firmware()`, `ota_request_rollback()`
- Pluggable transport interface (`ota_transport_t`)

**Missing for v1.6:**
1. `akira_boot_guard` — confirms a new image after a healthy boot and forces a reset when it crashes or hangs, so MCUboot rolls it back (section 4).
2. Manifest-based HTTP fetch — JSON manifest → URL → streaming download into MCUboot secondary slot.
3. Delta patching — streaming bspatch applied between the running slot and the incoming binary.
4. WASM native API — `wasm_ota_check()`, `wasm_ota_fetch_and_apply()`, `wasm_ota_get_state()`.

---

## 3. MCUboot Slot Architecture

```
Flash map (typical):
  ┌──────────────────────────────────────────┐
  │  boot partition       (MCUboot loader)   │
  ├──────────────────────────────────────────┤
  │  slot0_partition      (primary — running)│
  ├──────────────────────────────────────────┤
  │  slot1_partition      (secondary — OTA / │
  │                        previous firmware)│
  ├──────────────────────────────────────────┤
  │  storage              (settings, LittleFS)│
  └──────────────────────────────────────────┘
```

MCUboot must run in a **swap** mode (swap-using-move on ESP32 and STM32).
`ota_finalize_update()` marks the downloaded image with
`boot_request_upgrade(BOOT_UPGRADE_TEST)`; on the next reset MCUboot swaps it
into slot0 and boots it once as a *trial*. If the trial image resets without
calling `boot_write_img_confirmed()`, MCUboot swaps the previous firmware back.
After a confirmed swap, slot1 holds the previous firmware until the next
download overwrites it.

Overwrite-only mode (`CONFIG_BOOT_UPGRADE_ONLY`) copies the new image over the
old one, so nothing can be rolled back. MCUboot's Espressif SoC defaults pick
it; `build.sh` overrides it with `boards/mcuboot-swap-move.conf` for the Xtensa
parts (ESP32, ESP32-S2, ESP32-S3). The RISC-V parts (C3/C6/H2) stay
overwrite-only for now: their bootloader has no IRAM headroom and swap mode is
not validated there. Sysbuild builds already pass the swap mode to MCUboot.

---

## 4. Boot Guard Design (`akira_boot_guard`)

MCUboot only reverts an image that *resets* before it is confirmed. The boot
guard makes sure every failure ends in a reset and a healthy image gets
confirmed. It is on by default wherever `CONFIG_MCUBOOT_IMG_MANAGER=y`: in swap
mode an image nobody confirms would be rolled back on its next reset.

### 4.1 Trial boot

```
SYS_INIT (APPLICATION, after settings)  akira_boot_guard_init()
   │
   ├─ boot_is_img_confirmed() → confirmed image
   │     └─ "ota/trial" record present and names another image?
   │           → the previous trial failed and MCUboot rolled back:
   │             log it, erase the failed image's header in slot1,
   │             emit AKIRA_HOOK_OTA_ERROR (ota.error = -ECANCELED) at BOOT_READY
   │
   └─ not confirmed → trial boot
         ├─ store "ota/trial" = "<version>/<size>" of the trial image
         └─ start the deadline timer (AKIRA_BOOT_GUARD_TRIAL_TIMEOUT_S)

AKIRA_HOOK_BOOT_READY (end of akira_start())
   └─ trial → confirm after AKIRA_BOOT_GUARD_CONFIRM_DELAY_S of uptime
```

| Failure during the trial | What resets the device |
|--------------------------|------------------------|
| Crash (fatal error) | `k_sys_fatal_error_handler()` reboots — the guard's own handler (`AKIRA_BOOT_GUARD_FATAL_REBOOT`) or `AKIRA_PANIC` |
| Hang, BOOT_READY never reached | Deadline timer (runs from the timer interrupt, so a deadlocked thread cannot block it) |
| Hang with interrupts locked | Hardware watchdog (`CONFIG_AKIRA_WDT`) |
| Product-specific health check fails | `akira_boot_guard_reject()` |

### 4.2 Confirmation

By default the image is confirmed `AKIRA_BOOT_GUARD_CONFIRM_DELAY_S` seconds
after `AKIRA_HOOK_BOOT_READY`. With `CONFIG_AKIRA_BOOT_GUARD_MANUAL_CONFIRM=y`
the product confirms itself — `akira_boot_guard_confirm()` from C, `ota_confirm()`
from a WASM app with `AKIRA_CAP_OTA_TRIGGER`, or `ota confirm` in the shell —
once its own checks pass (network joined, first sensor read, ...). Every
confirm path ends the trial: it writes the MCUboot flag, stops the deadline,
clears the record and emits `AKIRA_HOOK_OTA_CONFIRMED`.

Firmware that does not boot through `akira_start()` must emit
`AKIRA_HOOK_BOOT_READY` itself or use manual confirmation; otherwise every
update rolls back at the deadline.

### 4.3 Manual rollback

`ota_request_rollback()` (shell `ota rollback`, WASM `ota_rollback()`):

- trial image: reboot, MCUboot restores the previous firmware;
- confirmed image: if slot1 still holds the previous firmware (no download
  since the last swap, not a discarded failed image), mark it as a trial with
  `boot_request_upgrade(BOOT_UPGRADE_TEST)` and reboot. MCUboot verifies it
  before swapping, and the boot guard handles it like any other trial;
- otherwise `OTA_ERROR_INVALID_IMAGE`, and no reboot.

The reboot is delayed by one second so the caller can report the result.

### 4.4 Kconfig

| Option | Default | Meaning |
|--------|---------|---------|
| `AKIRA_BOOT_GUARD` | `y` (needs `MCUBOOT_IMG_MANAGER`) | Enable the guard |
| `AKIRA_BOOT_GUARD_CONFIRM_DELAY_S` | 30 | Healthy uptime after BOOT_READY before confirming |
| `AKIRA_BOOT_GUARD_MANUAL_CONFIRM` | `n` | Leave confirmation to the product |
| `AKIRA_BOOT_GUARD_TRIAL_TIMEOUT_S` | 300 | Reboot (and roll back) if not confirmed by then |
| `AKIRA_BOOT_GUARD_INIT_PRIORITY` | 60 | SYS_INIT priority; must follow settings |
| `AKIRA_BOOT_GUARD_FATAL_REBOOT` | `y` unless `AKIRA_PANIC` | Reboot on a fatal error during a trial |

---

## 5. Delta Update Design (`akira_delta`)

### 5.1 Motivation

Typical ESP32-S3 firmware: ~400 KB.  A delta patch between two minor releases
is typically 20–60 KB — reducing OTA download by 80–90%.

### 5.2 Algorithm

**bsdiff/bspatch** (BSD licence, well-tested, widely deployed):
- Produces compressed patches using bzip2.
- Patch is applied in-place streaming: read old slot, apply patch blocks,
  write to secondary slot, never hold the full binary in RAM.

**Streaming patch flow:**

```
HTTP GET /firmware.patch
   │
   │  (chunk by chunk, CONFIG_AKIRA_OTA_DELTA_CHUNK_SIZE)
   ▼
akira_delta_open(ctx, secondary_slot_fd, primary_slot_fd)
akira_delta_feed(ctx, patch_chunk, len)   // called per HTTP chunk
akira_delta_close(ctx)                    // finalises secondary slot
   │
   ▼
ota_finalize_update()   // SHA-256 verify + boot_request_upgrade()
```

### 5.3 Memory Budget

- `akira_delta_ctx_t` heap usage: ~4 KB (bzip2 decompressor state).
- Stack delta: ≤ 512 B added to the OTA thread stack.
- No extra thread.

### 5.4 Kconfig

```kconfig
config AKIRA_OTA_DELTA
    bool "Delta (binary-patch) OTA updates"
    default n
    depends on AKIRA_OTA
    select CBPRINTF_FP_SUPPORT   # not needed but kept for reference

config AKIRA_OTA_DELTA_CHUNK_SIZE
    int "Delta patch input chunk size (bytes)"
    default 4096
    range 512 65536
    depends on AKIRA_OTA_DELTA
```

---

## 6. Manifest Format

```json
{
  "version": "1.6.0",
  "board":   "esp32s3_devkitm",
  "url":     "https://cdn.akirasystems.io/fw/akiraos-1.6.0.bin",
  "size":    425984,
  "sha256":  "a1b2c3...",
  "delta":   {
    "from_version": "1.5.4",
    "url":          "https://cdn.akirasystems.io/fw/akiraos-1.5.4-to-1.6.0.patch",
    "size":         38912,
    "sha256":       "d4e5f6..."
  }
}
```

- `delta` key is optional.  If present and `from_version` matches the running image, the delta path is preferred.
- SHA-256 verified client-side before calling `ota_finalize_update()`.
- JSON parsed with the existing `simple_json.c` helper.

---

## 7. WASM Native API (`akira_ota_api`)

### 7.1 Exports

| WASM function             | Signature (type string) | Description |
|---------------------------|-------------------------|-------------|
| `wasm_ota_check`          | `"($)i"`                | Fetch manifest from URL, return 1 if update available, 0 if up-to-date |
| `wasm_ota_fetch_and_apply`| `"($)i"`                | Download and stage firmware (blocking, progress via telemetry) |
| `wasm_ota_get_state`      | `"()i"`                 | Return `ota_state` enum value |
| `wasm_ota_confirm`        | `"()i"`                 | Confirm running firmware (clears boot guard) |
| `wasm_ota_rollback`       | `"()i"`                 | Request immediate rollback |

### 7.2 Capability

`AKIRA_CAP_OTA_TRIGGER` (bit 31) — must be declared in app manifest.

### 7.3 Security Considerations

- URL validation: only HTTPS allowed.  Reject `http://` at the API boundary.
- SHA-256 verified before any `boot_request_upgrade()` call.
- WASM sandbox cannot write directly to flash slots — only via the native OTA API.
- No URL or manifest content reaches WASM memory; only status codes.

---

## 8. Acceptance Criteria

| # | Criterion | Test |
|---|-----------|------|
| AC1 | A trial image is confirmed after BOOT_READY; a failed trial is reported as a rollback on the next boot | `tests/src/test_boot_guard.c` |
| AC2 | Delta patch produces identical binary to full-image download | `tests/ota/test_delta.c` |
| AC3 | `wasm_ota_get_state()` returns correct enum | `tests/wasm_api/test_ota_api.c` |
| AC4 | OTA without `AKIRA_CAP_OTA_TRIGGER` returns `-EACCES` | `tests/wasm_api/test_ota_api.c` |
| AC5 | Native sim test build compiles the boot guard and runs AC1 | CI |

---

## 9. Flash / RAM Cost Estimate

| Module | Flash (text) | RAM (bss/data) |
|--------|-------------|----------------|
| `akira_boot_guard` | ~1.5 KB | ~100 B (timer, work item) |
| `akira_delta` | ~8.0 KB (bzip2 subset) | ~4.1 KB (ctx, heap-allocated) |
| `akira_ota_api` | ~1.0 KB | ~0 B |
| Total (all enabled) | **~10.2 KB** | **~4.2 KB** |

Delta module is disabled by default (`CONFIG_AKIRA_OTA_DELTA=n`) so the
baseline cost is ~2.2 KB flash / ~128 B RAM for boot guard + API.

---

## 10. Out of Scope (v1.6)

- Code-signing key rotation
- Multi-image updates (MCUboot multi-image is possible but adds complexity)
- BLE OTA transport (already exists via `ota_ble_transport` in v1.5.x)
- A/B app bundle updates (WASM `.wasm` file updates handled separately)

---

*Last updated: 2026-09-14 (AkiraOS v1.6.5)*
