---
title: System Hooks
parent: API Reference
nav_order: 6
---

# System Hooks

A hook handler runs when a system event occurs — an app starts or crashes,
connectivity changes, an OTA image is staged. Product firmware uses hooks to
extend AkiraOS without editing the boot sequence or guessing `SYS_INIT`
priorities. Define one with `AKIRA_HOOK_DEFINE()` from `include/akira_hooks.h`:

```c
#include <akira_hooks.h>

static void on_event(const struct akira_hook_event *e, void *user)
{
    if (e->type == AKIRA_HOOK_BOOT_READY) {
        acme_start();
    }
}

AKIRA_HOOK_DEFINE(acme_hooks,
                  AKIRA_HOOK_MASK(AKIRA_HOOK_BOOT_READY) |
                      AKIRA_HOOK_MASK(AKIRA_HOOK_APP_CRASHED),
                  on_event, NULL);
```

The handler is called synchronously, on the thread that raised the event. It
must not block, and must not call back into the subsystem that raised the event.

## Events

| Event | When | Thread | Payload |
|---|---|---|---|
| `AKIRA_HOOK_BOOT_PRE_RUNTIME` | HAL and connectivity are up, before the WASM runtime | main | — |
| `AKIRA_HOOK_BOOT_READY` | app manager is up; ready to run apps | main | — |
| `AKIRA_HOOK_APP_INSTALLED` | an app was installed (and admitted) | installer (HTTP/BLE/USB/shell/fleet client) | `app.name`, `app.registry_id`, `app.version` |
| `AKIRA_HOOK_APP_UNINSTALLED` | an app was uninstalled | caller | `app.name`, `app.registry_id` |
| `AKIRA_HOOK_APP_STARTED` | an app started | caller | `app.name`, `app.registry_id`, `app.container_id` |
| `AKIRA_HOOK_APP_STOPPED` | an app stopped or exited cleanly | WASM app thread | `app.name`, `app.registry_id`, `app.container_id`, `app.exit_code` |
| `AKIRA_HOOK_APP_CRASHED` | an app trapped or exited non-zero | WASM app thread | as `APP_STOPPED` |
| `AKIRA_HOOK_APP_FAILED` | a crash exhausted the app's restart budget (raised right after `APP_CRASHED`) | WASM app thread | `app.name`, `app.registry_id`, `app.exit_code` |
| `AKIRA_HOOK_NET_UP` / `_DOWN` | Wi-Fi got an IPv4 address / disconnected | net_mgmt | `net.link` |
| `AKIRA_HOOK_BT_CONNECTED` / `_DISCONNECTED` | a BLE central connected/disconnected | BT RX | — |
| `AKIRA_HOOK_USB_CONFIGURED` / `_DISCONNECTED` | the host configured the device / VBUS was removed | USB | — |
| `AKIRA_HOOK_OTA_STARTED` | the secondary slot was erased for a new image | OTA worker | `ota.state` |
| `AKIRA_HOOK_OTA_STAGED` | the image passed size/hash/version checks and is marked for a test boot | OTA worker | `ota.state` |
| `AKIRA_HOOK_OTA_PRE_REBOOT` | `ota_reboot_to_apply_update()` / `akira_ota_reboot()` is about to reboot | caller | — |
| `AKIRA_HOOK_OTA_CONFIRMED` | the running trial image was confirmed | boot guard / caller | — |
| `AKIRA_HOOK_OTA_ERROR` | an update failed (flash, hash, downgrade, boot request), or the previous boot rolled back (`ota.error = -ECANCELED`, at `BOOT_READY`) | OTA worker / main | `ota.state`, `ota.error` (an `enum ota_result` or `-ECANCELED`) |
| `AKIRA_HOOK_BT_PASSKEY_DISPLAY` | a phone is pairing by passkey entry (`CONFIG_AKIRA_BT_MGMT_PAIRING_PASSKEY`, any mode but HID): show the six digits until `BT_PAIRING_DONE` | BT RX | `bt.passkey` (0–999999, show zero-padded) |
| `AKIRA_HOOK_BT_PAIRING_DONE` | that pairing completed, failed or was cancelled | BT RX | `bt.paired` |

Handlers on the WASM app thread (stopped, crashed) and the BT RX thread run with
small stacks (8 KB and ~2 KB on akiraconsole_prod); keep them short.

Hooks only observe. To *decide* (refuse an app, block a connection, supply
a key), use the provider APIs in
[Extension points](extension-points.md).

## Relationship to the legacy weak hooks

The `akira_on_app_installed/uninstalled/started/crashed` weak symbols
(`akira_platform_stubs.h`) that AkiraPlatform overrides still fire, through a
built-in hook that forwards those four events. They are deprecated; new code
should define its own hook. App-lifecycle events are still mirrored to the WASM
`akira.lifecycle` IPC topic for apps.
