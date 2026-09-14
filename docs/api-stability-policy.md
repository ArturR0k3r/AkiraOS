---
layout: default
title: API Stability Policy
parent: API Reference
nav_order: 4
---

# AkiraOS API Stability Policy

Every public header in AkiraOS carries two Doxygen tags that declare its
contractual stability:

```c
/**
 * @stability stable          // or: experimental | deprecated
 * @since 1.3                 // version in which this API first appeared
 */
```

---

## Stability Levels

### `stable`

The API is production-ready and covered by a **two-release deprecation
notice** before any breaking change or removal.

Rules:
- Signatures, struct layouts, and error codes **will not change** without
  first issuing a `deprecated` notice in the immediately preceding release.
- A deprecated API **must** remain available and functional for at least
  **two minor releases** after the deprecation notice.
- Breaking changes in a stable API require a **major version bump**.
- Renaming a stable symbol requires a compatibility alias for two releases.

### `experimental`

The API exists and is usable, but **no compatibility guarantee** is made.

Rules:
- May change signature, semantics, or be removed **in any release** without
  prior notice.
- Will not be documented in the main Getting Started guide until promoted.
- Callers must opt in by defining `AKIRA_ACCEPT_EXPERIMENTAL` or by
  including the header explicitly.  
  *(Feature flag enforcement will land in v1.6.)*
- Promotion to `stable` requires: two releases without API change, full test
  coverage, and a public RFC issue.

### `deprecated`

The API is scheduled for removal and **must not be used in new code**.

Rules:
- A `deprecated` header or function emits a compile-time warning via
  `__attribute__((deprecated("use X instead")))`.
- Will be removed no sooner than **two minor releases** after the deprecation
  tag appears.
- The deprecation notice states the replacement symbol.
- Example annotation:

  ```c
  /**
   * @deprecated Since 1.5 — use akira_storage_open() instead.
   */
  __attribute__((deprecated("use akira_storage_open()")))
  int legacy_fs_open(const char *path, int flags);
  ```

---

## Versioning Alignment

AkiraOS uses **Semantic Versioning** (`MAJOR.MINOR.PATCH`):

| Bump | Meaning |
|------|---------|
| PATCH | Bug fixes; no API changes. |
| MINOR | New stable APIs added; backward-compatible. Deprecated APIs may be introduced. |
| MAJOR | Breaking changes to stable APIs; deprecated APIs removed. |

The `@since X.Y` tag records the **minor version** in which the API first
became available (patch is omitted because patch releases never add APIs).

---

## Process: Promoting `experimental` → `stable`

1. Open a GitHub issue titled `[RFC] Promote <header> to stable`.
2. API must have been **unchanged for two consecutive minor releases**.
3. Requires **≥ 90% line coverage** in the test suite.
4. At least one **board other than native_sim** must pass CI with the new
   stable designation.
5. PR adds `@stability stable` and updates this document's table below.

---

## Current API Surface (v1.6.4)

| Header | Stability | Since |
|--------|-----------|-------|
| `include/akira_native_api.h` | stable | 1.4 |
| `include/lib/akira_time.h` | stable | 1.3 |
| `include/modules/akira_log_module.h` | stable | 1.4 |
| `include/modules/akira_time_module.h` | stable | 1.4 |
| `include/connectivity/radio_interface.h` | experimental | 1.5 |
| `include/connectivity/akira_mesh.h` | experimental | 1.5 |
| `include/connectivity/matter_manager.h` | experimental | 1.5 |
| `include/connectivity/thread_manager.h` | experimental | 1.5 |
| `src/akira.h` | stable | 1.0 |
| `src/api/akira_api.h` | stable | 1.0 |
| `src/api/akira_common_api.h` | stable | 1.0 |
| `src/api/akira_display_api.h` | stable | 1.2 |
| `src/api/akira_memory_api.h` | stable | 1.2 |
| `src/api/akira_gpio_api.h` | stable | 1.3 |
| `src/api/akira_storage_api.h` | stable | 1.3 |
| `src/api/akira_timer_api.h` | stable | 1.3 |
| `src/api/akira_adc_api.h` | stable | 1.4 |
| `src/api/akira_ble_api.h` | stable | 1.4 |
| `src/api/akira_i2c_api.h` | stable | 1.4 |
| `src/api/akira_lifecycle_api.h` | stable | 1.4 |
| `src/api/akira_net_api.h` | stable | 1.4 |
| `src/api/akira_power_api.h` | stable | 1.4 |
| `src/api/akira_pwm_api.h` | stable | 1.4 |
| `src/api/akira_sensor_api.h` | stable | 1.4 |
| `src/api/akira_settings_api.h` | stable | 1.4 |
| `src/api/akira_system_api.h` | stable | 1.4 |
| `src/api/akira_uart_api.h` | stable | 1.4 |
| `src/api/akira_wdt_api.h` | stable | 1.4 |
| `src/api/akira_hid_api.h` | experimental | 1.4 |
| `src/api/akira_ipc_api.h` | experimental | 1.4 |
| `src/api/akira_rf_api.h` | experimental | 1.4 |
| `src/runtime/security.h` | stable | 1.3 |
| `src/runtime/security/app_signing.h` | stable | 1.3 |
| `src/runtime/security/sandbox.h` | stable | 1.3 |
| `src/runtime/security/trust_levels.h` | stable | 1.3 |
| `src/runtime/app_manager/app_manager.h` | stable | 1.4 |
| `src/runtime/manifest_parser.h` | stable | 1.4 |
| `src/runtime/akira_runtime.h` | stable | 1.3 |
| `src/connectivity/ota/ota_manager.h` | stable | 1.4 |
| `src/lib/error_codes.h` | stable | 1.0 |
| `src/lib/mem_helper.h` | stable | 1.2 |
| `src/lib/path_utils.h` | stable | 1.3 |
| `src/lib/simple_json.h` | stable | 1.4 |
| `src/shell/akira_shell.h` | stable | 1.3 |
| `src/storage/fs_manager.h` | stable | 1.3 |
| `src/drivers/platform_hal.h` | stable | 1.2 |
| `src/drivers/driver_registry.h` | stable | 1.3 |
| `src/drivers/wdt/akira_wdt.h` | stable | 1.4 |
| `src/drivers/power/power_manager.h` | stable | 1.4 |
| `src/drivers/sensors/button.h` | stable | 1.3 |
| All connectivity/cloud/* headers | experimental | 1.5 |
| All connectivity/hid/* headers | experimental | 1.4 |
| All connectivity/bluetooth/* headers | experimental | 1.4 |
| All drivers/rf/* headers | experimental | 1.4 |
| All drivers/display/* headers | experimental | 1.4 |
| All drivers/sensors/* (except button) | experimental | 1.4 |

---

## Deprecation Example Workflow

**Release 1.5.x:** Introduce replacement, mark old API:
```c
/** @deprecated Since 1.5 — use akira_power_get_level() instead. */
__attribute__((deprecated("use akira_power_get_level()")))
int legacy_battery_read(void);
```

**Release 1.6.0:** Warning still emitted; migration guide published.

**Release 1.7.0:** API removed; CHANGELOG entry under `### Removed`.

---

*Last updated: 2026-09-14 (AkiraOS v1.6.4)*
