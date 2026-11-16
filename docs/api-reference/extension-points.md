---
title: Extension Points
parent: API Reference
nav_order: 7
---

# Extension Points for Product Firmware

AkiraOS is a Zephyr module ([Using AkiraOS as a west module](../hardware/west-module.md)).
Product firmware and add-on modules extend it through the public headers in
`include/`. This page lists them; the headers carry the full contracts.

Since 1.7 these headers are the whole supported surface for out-of-tree code.
Headers under `src/` are internal and may change in any release. All
extension-point headers are `@stability experimental` (see the
[API stability policy](../api-stability-policy.md)).

## Observe: hooks

`akira_hooks.h`: `AKIRA_HOOK_DEFINE()` subscribes to boot, app-lifecycle,
network, Bluetooth, USB and OTA events. Hooks only observe. See
[System Hooks](hooks.md).

## Decide: providers

Providers are registered at link time with a `*_DEFINE()` macro, like hooks.
Unlike hooks, a provider's return value changes what AkiraOS does.

| Header | Macro | Decides | Called from |
|---|---|---|---|
| `akira_admission.h` | `AKIRA_APP_ADMISSION_DEFINE(id, prio, fn, user)` | whether an app may be installed / loaded; whether it is *attested*; capability clamps | installing thread; every load |
| `akira_net_policy.h` | `AKIRA_NET_POLICY_DEFINE(id, fn, user)` | whether a WASM app may connect, bind, listen, or make the firmware fetch a URL | the app's thread |
| `akira_keyprovider.h` | `AKIRA_SETTINGS_KEY_PROVIDER_DEFINE(id, prio, fn, user)` | the AES-256 key for encrypted settings | settings `SYS_INIT` |
| `akira_identity.h` | `AKIRA_IDENTITY_PROVIDER_DEFINE(id, fn, user)` | the provisioned serial, organisation and certificate fingerprint shown by `akira version` and `GET /api/v1/info` | caller of `akira_identity_get()` |
| `akira_audit.h` | `AKIRA_AUDIT_SINK_DEFINE(id, fn, user)` | where audit records go (persist, sign, upload) | system workqueue |

### App admission

Every install path (HTTP, BLE, USB, SD, shell, a fleet client) calls the
admission providers before writing to flash. Every load calls them again,
including SD-XIP and cloud loads that skip the app manager. Providers run in
ascending priority order:

- One `DENY` refuses the app.
- An app is **attested** when a provider returns `ALLOW` with `res->attested`.
  Attested apps may receive privileged capabilities and, with
  `CONFIG_AKIRA_HARDEN_UNATTESTED_APPS=y`, run as AOT.
- Capability clamps from all providers are ANDed.
- `CONFIG_AKIRA_REQUIRE_SIGNED_APPS=y` refuses any app nobody attested.

At install the core writes an **admission record** next to the app
(`/lfs/apps/NNN_name.adm`). It holds the SHA-256 of the binary and of the
stored manifest, the verdict, the clamp, the signer and an optional
provider-computed seal. A later load whose binary or manifest differs from the
record is refused before any provider runs. A provider that needs the record
to resist an attacker with filesystem write access fills `seal` at install
(e.g. an HMAC with a hardware key) and checks it at load.

**Baseline provider.** AkiraOS ships one provider (priority 100): the OEM key in
`CONFIG_AKIRA_APP_PUBKEY` (Ed25519, 64 hex digits). `akira-cli sign` puts
`sig.ed25519` = Ed25519(SHA-256(manifest.json ‖ app binary ‖ model.tflite))
into the `.akpkg`. A package that verifies is attested. At load, the record
re-attests only if it names the key currently configured. A signature that
does not verify is not refused by this provider (another provider may trust
it); `CONFIG_AKIRA_REQUIRE_SIGNED_APPS=y` refuses it if nothing else does.

Bare `.wasm` uploads carry no signature, so they are never attested.

### Network egress

`akira_net_policy_check()` runs before `net_connect`, `net_bind`,
`net_listen`, and before `ota_check` / `ota_fetch_and_apply` fetch the
manifest or image URL for an app. With no provider, everything is allowed (the
`network.use` and `ota.trigger` capabilities still apply). The manifest's
`network_policy` reaches providers in the admission load context
(`ctx->net_policy`); AkiraOS does not enforce it by itself.

### Settings key

The first provider (by priority) that returns 0 supplies the key. `-ENOTSUP`
and `-ENODEV` pass to the next provider. Any other error fails settings
initialisation rather than falling back to a weaker key. Without a provider
the key is derived from `CONFIG_AKIRA_SETTINGS_ENCRYPTION_KEY_HEX` and the
hardware ID.

### Audit

`akira_audit_emit()` adds a record (ISR-safe). Sinks receive every record in
sequence order, outside the audit lock. A sink that falls behind the ring gets
one `AKIRA_AUDIT_DROPPED` record that counts what it missed.

## Act: services

| Header | What |
|---|---|
| `akira_os.h` | `akira_start()`: the standard boot sequence |
| `akira_apps.h` | list, install (`.akpkg`), start, stop, uninstall apps |
| `akira_ota.h` | write and stage a firmware image; `akira_boot_guard.h` decides confirm or rollback |
| `akira_wifi.h` | station credentials, scan, soft-AP |
| `akira_mem.h` | PSRAM-first allocation for large buffers |
| `akira_ed25519.h` | Ed25519 signature verification |
| `akira_capability.h` | product capabilities (`AKIRA_CAPABILITY_DEFINE`), `AKIRA_CHECK_CAP_OR_RETURN()` for native functions |
| `akira_native_registry.h` | export native functions to WASM (`AKIRA_NATIVE_API_DEFINE`) |
| `akira_identity.h` | versions, board, device ID, provisioned identity |
| `akira_boot_guard.h` | trial-image confirm / reject |

## Linking rule

Registry entries (`*_DEFINE`) must be compiled into the `app` target or into
a `zephyr_library()` created by a **Zephyr module**. Zephyr links those
whole-archive. A `zephyr_library()` that an application creates after
`find_package(Zephyr)` is not whole-archived, and an object holding only
registry entries is silently dropped from it. See
[Using AkiraOS as a west module](../hardware/west-module.md).
