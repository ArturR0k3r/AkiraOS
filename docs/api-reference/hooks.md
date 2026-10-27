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
| `AKIRA_HOOK_APP_INSTALLED` | an app was installed | installer (HTTP/BLE/USB/shell) | `app.name`, `app.registry_id`, `app.version` |
| `AKIRA_HOOK_APP_UNINSTALLED` | an app was uninstalled | caller | `app.name` |
| `AKIRA_HOOK_APP_STARTED` | an app started | caller | `app.name`, `app.container_id` |
| `AKIRA_HOOK_APP_STOPPED` | an app stopped or exited cleanly | WASM app thread | `app.name`, `app.exit_code` |
| `AKIRA_HOOK_APP_CRASHED` | an app trapped or exited non-zero | WASM app thread | `app.name`, `app.exit_code` |
| `AKIRA_HOOK_APP_FAILED` | an app exhausted its restart budget | app manager | `app.name` |
| `AKIRA_HOOK_NET_UP` / `_DOWN` | a network link changed | net_mgmt | `net.link` |
| `AKIRA_HOOK_BT_CONNECTED` / `_DISCONNECTED` | a BLE central connected/disconnected | BT RX | — |
| `AKIRA_HOOK_USB_CONFIGURED` / `_DISCONNECTED` | USB was (de)configured | USB | — |
| `AKIRA_HOOK_OTA_STARTED` / `_STAGED` / `_PRE_REBOOT` / `_CONFIRMED` / `_ERROR` | OTA progress | OTA | `ota.state`, `ota.error` |

Handlers on the WASM app thread (stopped, crashed) and the BT RX thread run with
small stacks (8 KB and ~2 KB on akiraconsole_prod); keep them short.

## Relationship to the legacy weak hooks

The `akira_on_app_installed/uninstalled/started/crashed` weak symbols
(`akira_platform_stubs.h`) that AkiraPlatform overrides still fire, through a
built-in hook that forwards those four events. They are deprecated; new code
should define its own hook. App-lifecycle events are still mirrored to the WASM
`akira.lifecycle` IPC topic for apps.
