# AkiraOS OTA Update System — Design Document

**Version:** 1.6 (draft)  
**Status:** Approved for implementation  
**Scope:** Track A — OTA with atomic rollback and delta updates

---

## 1. Goals

| # | Goal |
|---|------|
| G1 | Atomic slot-swap: only boot new firmware after explicit confirmation |
| G2 | Automatic rollback after N consecutive boot failures (configurable) |
| G3 | Delta (binary-patch) update mode to reduce download size |
| G4 | WASM app can trigger / monitor OTA without elevated native access |
| G5 | Single `docs/feature_overhead.md` entry tracking flash/RAM cost |

---

## 2. Current State (v1.5.4)

`src/connectivity/ota/ota_manager.c/h` already provides:
- MCUboot `boot_request_upgrade()` / `boot_write_img_confirmed()` wrappers
- `ota_start_update()`, `ota_write_chunk()`, `ota_finalize_update()`
- `ota_confirm_firmware()`, `ota_request_rollback()`
- Pluggable transport interface (`ota_transport_t`)

**Missing for v1.6:**
1. `akira_boot_guard` — persistent boot counter stored in NVS, triggers rollback if the counter exceeds `CONFIG_AKIRA_OTA_MAX_BOOT_FAILURES` without being cleared.
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
  │  slot1_partition      (secondary — OTA)  │
  ├──────────────────────────────────────────┤
  │  panic_store          (NVS — crash log)  │
  ├──────────────────────────────────────────┤
  │  boot_counter         (NVS — boot count) │  ← NEW
  └──────────────────────────────────────────┘
```

MCUboot performs the swap atomically: if the new image does not call
`boot_write_img_confirmed()` within `CONFIG_MCUBOOT_WATCHDOG_FEED_TIMEOUT_MS`,
it reverts to the previous primary slot on next reset.

AkiraOS adds a software-level boot guard **on top** of MCUboot to handle the
case where the device crashes during application startup before the RTOS
watchdog feeds MCUboot.

---

## 4. Boot Guard Design (`akira_boot_guard`)

### 4.1 Data Layout

A small NVS partition (`boot_counter`) holds a single record:

```c
typedef struct {
    uint32_t magic;          // 0xB007C0DE when valid
    uint8_t  boot_count;     // incremented on every unconfirmed boot
    uint8_t  confirmed;      // set to 1 after akira_boot_guard_confirm()
    uint8_t  _pad[2];
} akira_boot_counter_t;
```

### 4.2 Boot Sequence

```
akira_boot_guard_init()
   │
   ├─ Read NVS record
   │   ├─ Not found → write {magic, boot_count=1, confirmed=0}
   │   └─ Found
   │       ├─ confirmed=1 → reset to {confirmed=0, boot_count=1} → OK
   │       └─ confirmed=0
   │           ├─ boot_count < MAX → increment, write back → continue
   │           └─ boot_count >= MAX → LOG_ERR, call ota_request_rollback() → reboot
   │
   └─ Return 0 (continue boot) or -EAGAIN (rollback triggered)
```

### 4.3 Confirmation

The application must call `akira_boot_guard_confirm()` once it is healthy
(e.g., after successful network join + first sensor read). This sets
`confirmed=1` and calls `ota_confirm_firmware()` to also confirm at the
MCUboot level.

### 4.4 Kconfig

```kconfig
config AKIRA_BOOT_GUARD
    bool "AkiraOS software boot guard"
    default n
    select AKIRA_OTA
    select NVS

config AKIRA_OTA_MAX_BOOT_FAILURES
    int "Max consecutive unconfirmed boots before rollback"
    default 3
    range 1 10
    depends on AKIRA_BOOT_GUARD
```

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
| AC1 | Three consecutive unconfirmed boots trigger rollback | `tests/ota/test_boot_guard.c` |
| AC2 | Delta patch produces identical binary to full-image download | `tests/ota/test_delta.c` |
| AC3 | `wasm_ota_get_state()` returns correct enum | `tests/wasm_api/test_ota_api.c` |
| AC4 | OTA without `AKIRA_CAP_OTA_TRIGGER` returns `-EACCES` | `tests/wasm_api/test_ota_api.c` |
| AC5 | Native sim build with `CONFIG_AKIRA_BOOT_GUARD=y` compiles and runs AC1 | CI |

---

## 9. Flash / RAM Cost Estimate

| Module | Flash (text) | RAM (bss/data) |
|--------|-------------|----------------|
| `akira_boot_guard` | ~1.2 KB | ~128 B (NVS fs struct) |
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
