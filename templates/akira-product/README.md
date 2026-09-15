# AkiraOS product template

Starting point for firmware built on AkiraOS **without forking it**. Copy this
directory to a new repository; it consumes AkiraOS as a Zephyr module.

```
west.yml                 # imports akira-os (pin a release tag), which imports Zephyr/WAMR/TFLM
prj.conf                 # CONFIG_AKIRA_OS=y + your product options
CMakeLists.txt           # AKIRA_SNIPPETS = profile + akira-board; links akira_os
app/src/main.c           # product setup, then akira_start()
subsys/widget/           # your subsystem: vendor capability + native API + hook
boards/                  # your custom board (AkiraOS boards stay available)
.github/workflows/ci.yml # build against the pinned AkiraOS
```

## Bring it up

```bash
west init -m https://github.com/your-org/widget-fw && west update
west build -b <your-board> product && west flash
```

## The four extension points (see `subsys/widget/widget.c`)

- **Capabilities** — `AKIRA_CAPABILITY_DEFINE("widget.actuate", 48, ...)` claims
  a vendor bit (48-63); apps request it by name in their manifest.
- **Native APIs** — `AKIRA_NATIVE_API_DEFINE(widget_api, "env", ...)` exports
  functions to WASM apps, each gated with `AKIRA_CHECK_CAP_OR_RETURN`.
- **Hooks** — `AKIRA_HOOK_DEFINE(...AKIRA_HOOK_BOOT_READY...)` starts the product
  at the right point without editing the boot sequence.
- **Profiles** — `AKIRA_SNIPPETS` picks a service profile
  (`akira-profile-minimal|connected|console|sensor-node`) plus `akira-board`.

None of this requires changes inside `akira-os/`.
