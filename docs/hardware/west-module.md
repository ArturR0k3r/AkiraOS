---
layout: default
title: AkiraOS as a west module
parent: Hardware
nav_order: 2
permalink: /hardware/west-module
---

# Using AkiraOS as a west module

Product firmware lives in **its own repository** and consumes AkiraOS as a
Zephyr module — the same way a project consumes Zephyr, nRF Connect SDK or
ESP-IDF. You never fork AkiraOS, and you never edit files inside `akira-os/`.
This guide takes a new product repo from empty to a booting image and shows the
four extension points a product uses.

A complete, CI-built example is
[`samples/out_of_tree_product`](https://github.com/ArturR0k3r/AkiraOS/tree/v1.6.x/samples/out_of_tree_product);
a fuller scaffold with a subsystem is
[`templates/akira-product`](https://github.com/ArturR0k3r/AkiraOS/tree/v1.6.x/templates/akira-product).
Copy the template into your new repo to skip the boilerplate below.

{: .note }
The module interface (`akira_os` CMake target, `AKIRA_SNIPPETS`, the
`akira_capability.h` / `akira_native_registry.h` / `akira_hooks.h` /
`akira_abi.h` headers) is **experimental** in 1.6.x and may change in a minor
release. Pin an AkiraOS tag and read the release notes before upgrading.

## 1. The workspace

Your repository is the west *manifest repo*. `west.yml` pulls in AkiraOS, and
AkiraOS's own manifest (`import: true`) pulls in Zephyr, the WAMR fork and
TFLite Micro — you list only AkiraOS.

```yaml
# widget-fw/west.yml
manifest:
  remotes:
    - name: akira
      url-base: https://github.com/ArturR0k3r
  projects:
    - name: akira-os
      remote: akira
      revision: v1.6.x          # pin a release tag for a reproducible product
      path: akira-os
      import: true              # brings in zephyr, wasm-micro-runtime, tflite-micro
  self:
    path: widget-fw
```

```bash
west init -m https://github.com/your-org/widget-fw widget-workspace
cd widget-workspace
west update                      # fetches akira-os + zephyr + wamr + tflite-micro
west zephyr-export
west blobs fetch hal_espressif   # ESP32 targets only
```

Your tree ends up as:

```
widget-workspace/
  widget-fw/        <- your repo (manifest repo)
  akira-os/         <- the module, pinned to your tag
  zephyr/  modules/ <- pulled in by akira-os's import
```

## 2. The minimal product

Three files in `widget-fw/`:

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20.0)

# Your own boards live here; AkiraOS boards stay available from the module.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR})

# Pick an AkiraOS service profile + the per-board AkiraOS setup. Profile first
# so a board conf can override a profile default (see section 4).
set(AKIRA_SNIPPETS akira-profile-minimal akira-board)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(widget_fw)

target_sources(app PRIVATE src/main.c)
target_link_libraries(app PRIVATE akira_os)   # AkiraOS include paths
```

```kconfig
# prj.conf
CONFIG_AKIRA_OS=y      # every CONFIG_AKIRA_* option depends on this
```

```c
/* src/main.c */
#include <akira.h>

int main(void)
{
    /* product-specific setup here */
    return akira_start();   /* the standard AkiraOS boot sequence */
}
```

Build and flash for any AkiraOS board, or your own:

```bash
west build -b native_sim widget-fw            # host build, boots immediately
west build -b esp32s3_devkitm/esp32s3/procpu widget-fw && west flash
```

`akira_start()` brings up the HAL, storage, the WASM runtime and the app
manager, then runs the idle loop — the same boot path as the reference
firmware. If you need your own `main`, call `akira_start()` when you are ready,
or omit it and drive the subsystems yourself.

## 3. Choosing what AkiraOS builds — profiles

`AKIRA_SNIPPETS` selects a **service profile** and the **board setup**, both
shipped inside the module as Zephyr snippets:

| Profile | For | Turns on |
|---|---|---|
| `akira-profile-minimal` | run signed WASM apps with storage | WASM runtime + API, app manager, LittleFS/NVS, settings, signing |
| `akira-profile-connected` | networked product | minimal + IP stack, HTTP upload, mDNS, OTA over MCUboot |
| `akira-profile-console` | screen + buttons + BT device | app services for display, SD, WASM BLE/HID, on-device shell |
| `akira-profile-sensor-node` | headless sensor | minimal + sensor/ADC/watchdog WASM APIs |
| `akira-board` | **always** — per-board flash layout, storage nodes, tuning | (board-specific) |

List the profile first and `akira-board` last, so a board's own settings win.
The user can add more at build time; snippets passed with `west build -S` are
applied after yours and override them:

```bash
west build -b <board> widget-fw -S rtt-console
```

{: .warning }
On boards whose AkiraOS setup enables networking or other services, the slim
`akira-profile-minimal` is not enough on its own — pair the board with a
matching profile (e.g. `akira-profile-connected`). If Kconfig aborts with
"unsatisfied dependencies", the profile and the board disagree about a service;
choose a profile that matches the board's hardware.

## 4. Adding a custom board

Put board files under `widget-fw/boards/` exactly as in a normal Zephyr project
(`board.yml`, `.dts`, `_defconfig`, `Kconfig.<board>`, `board.cmake`). The
`BOARD_ROOT` line in your `CMakeLists.txt` registers them. AkiraOS DTS bindings
(`akira,ili9341`, `akira,pwm-dial`, …) are available because the module adds its
`dts/` to the bindings path. See the
[Porting Guide](porting-guide.md) for bring-up.

Give the board its own AkiraOS setup by adding
`widget-fw/boards/<board>.conf` / `.overlay` and listing them, or reuse the
module's `akira-board` snippet for an AkiraOS board.

## 5. The four extension points

Everything a product adds registers itself through link-time tables — no edits
to AkiraOS. Put these in a subsystem compiled **into the application** (see the
gotcha below), for example `subsys/widget/widget.c`:

```c
#include <akira_capability.h>
#include <akira_native_registry.h>
#include <akira_hooks.h>
#include <runtime/security.h>

/* (a) A product capability, in the vendor range 48-63. Apps request it by name
 *     in their manifest ("widget.actuate"). A duplicate bit fails to link. */
#define WIDGET_CAP_ACTUATE BIT64(48)
AKIRA_CAPABILITY_DEFINE(widget_actuate, "widget.actuate", 48, AKIRA_CAPABILITY_PRIVILEGED);

/* (b) A native the product exports to WASM apps, gated on that capability. */
static int widget_native_actuate(wasm_exec_env_t exec_env, int32_t position)
{
    AKIRA_CHECK_CAP_OR_RETURN(exec_env, WIDGET_CAP_ACTUATE, -EPERM);
    /* drive the hardware */
    return 0;
}
static const NativeSymbol widget_natives[] = {
    { "widget_actuate", (void *)widget_native_actuate, "(i)i", NULL },
};
AKIRA_NATIVE_API_DEFINE(widget_api, "env", widget_natives);

/* (c) A hook: start the product once AkiraOS is ready — no SYS_INIT priority
 *     guessing, no editing the boot sequence. */
static void on_ready(const struct akira_hook_event *e, void *user)
{
    if (e->type == AKIRA_HOOK_BOOT_READY) { /* start product services */ }
}
AKIRA_HOOK_DEFINE(widget_boot, AKIRA_HOOK_MASK(AKIRA_HOOK_BOOT_READY), on_ready, NULL);
```

The fourth point is **profiles** (section 3). Core capability bit numbers (0–47)
are frozen; yours live in 48–63. See the
[API stability policy](../api-reference/api-stability-policy.md) and the
[capability list](../api-reference/manifest-format.md).

{: .warning }
**Compile subsystem sources into `app`, not a standalone `zephyr_library()`.**
The `AKIRA_CAPABILITY_DEFINE` / `AKIRA_NATIVE_API_DEFINE` / `AKIRA_HOOK_DEFINE`
entries live in linker iterable sections that are only kept when the object is
whole-archived. An application's own `zephyr_library()` is registered *after*
`find_package(Zephyr)` and misses Zephyr's whole-archive capture, so its entries
are silently dropped (your native never registers, your hook never fires).
Guard the sources with your Kconfig and add them to `app`:

```cmake
# subsys/widget/CMakeLists.txt
if(CONFIG_WIDGET)
  target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/widget.c)
endif()
```

A larger subsystem can instead ship as its own Zephyr module (with a
`zephyr/module.yml`), which *is* processed during `find_package` and keeps its
entries.

## 6. App ABI compatibility

WASM apps are built against an SDK release that targets a WASM import ABI
version (`akira_abi.h`). The runtime refuses an app whose major ABI differs from
the firmware and enforces the app's `min_akiraos_version`. Keep your product's
AkiraOS tag and the AkiraSDK release your apps are built with compatible; the
firmware CI check `scripts/check_wasm_abi.py` guards drift.

## 7. What you did *not* touch

`git -C akira-os status` stays clean: boards, capabilities, native APIs, hooks
and services all came from your own repo. Upgrading AkiraOS is a `revision:`
bump in `west.yml` plus `west update`.

## Related

- [Porting Guide](porting-guide.md) — bring AkiraOS up on a new board
- [Building WASM Apps](../development/building-apps.md) — the app side
- [Manifest Format](../api-reference/manifest-format.md) — capabilities, `abi`, `min_akiraos_version`
