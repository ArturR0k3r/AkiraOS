---
layout: default
title: Debugging
parent: Development
nav_order: 3
---

# Debugging Guide

Debugging techniques for AkiraOS development.

## Logging

### Enable Debug Logs

```bash
CONFIG_LOG_DEFAULT_LEVEL=4  # DEBUG
```

### Runtime Log Control

```bash
AkiraOS:~$ log enable akira 4
AkiraOS:~$ log enable wasm 4
AkiraOS:~$ log list
```

## GDB Debugging (Native Sim)

```bash
# Build with debug symbols
west build -b native_sim -- -DCMAKE_BUILD_TYPE=Debug

# Run under GDB
gdb ../build/zephyr/zephyr.exe
(gdb) break main
(gdb) run
```

## JTAG Debugging (ESP32-S3)

```bash
# OpenOCD + GDB
west debug
```

## Core Dumps

Enable core dumps on crash:

```bash
CONFIG_DEBUG_COREDUMP=y
CONFIG_DEBUG_COREDUMP_BACKEND_LOGGING=y
```

## Memory Debugging

### Check Stack Usage

```bash
AkiraOS:~$ kernel stacks
AkiraOS:~$ kernel threads
```

### Enable Stack Canaries

```bash
CONFIG_STACK_CANARIES=y
```

## Shell Command Reference

AkiraOS registers the following root commands on the Zephyr shell. A command is only
present when its gating Kconfig option is enabled. Use `help` on the target, or `<cmd> -h`,
for per-command usage.

| Command | Gated by | Subcommands |
|---------|----------|-------------|
| `akira` | always | `apps`, `start`, `stop`, `mem`, `caps`, `ipc`, `ota`, `log`, `version` |
| `app` | `CONFIG_AKIRA_APP_MANAGER` | `list`, `info`, `start`, `stop`, `restart`, `uninstall`, `install`, `scan`, `run_sd` |
| `sys` | always | `info`, `stress`, `threads`, `reboot` |
| `debug` | always | `memdump`, `alias`, `history`, `clear_history`, `benchmark`, `shell_stats`, `hwtest` |
| `ram` | always | `ls`, `cat` |
| `settings` | `CONFIG_AKIRA_SETTINGS` | `get`, `set`, `list`, `delete`, `set_wifi`, `clear`, `info` |
| `display` | `CONFIG_DISPLAY` | `info`, `clear`, `fill`, `pixel`, `line`, `test`, `flush`, `brightness`, `blanking`, `rotate` |
| `gpio` | `CONFIG_GPIO` | `read`, `configure` |
| `date` | always | `get`, `set` |
| `bt` | `CONFIG_BT` | `info`, `stats`, `addr`, `disconnect`, `unpair`, `gatt`, `echo`, `adv {start,stop,status}` |
| `hid` | `CONFIG_AKIRA_HID` | `enable`, `disable`, `transport`, `status`, `sim {connect,disconnect}`, `keyboard type` |
| `usb` | `CONFIG_AKIRA_USB` | `init`, `finalize`, `deinit`, `enable`, `disable`, `status`, `stats`, `wakeup`, `info` |
| `wifi_scan`, `wifi_connect`, `wifi_status` | `CONFIG_AKIRA_WIFI` | flat commands |
| `rf` | `CONFIG_AKIRA_MODULE_RF` | `init`, `deinit`, `select`, `freq`, `power`, `mod`, `bw`, `lora {sf,cr}`, `send`, `sweep`, `recv`, `capture`, `replay`, `rssi`, `status`, `test` |
| `radio` | `CONFIG_AKIRA_RADIO_MANAGER` | `info`, `stats` |
| `mesh` | `CONFIG_AKIRA_MESH` | `init`, `start`, `stop`, `send`, `info`, `nodes` |
| `mqtt` | `CONFIG_AKIRA_MQTT` | `set`, `connect`, `disconnect`, `status`, `pub` |
| `matter` | `CONFIG_AKIRA_MATTER` | `info`, `commission`, `reset`, `pair`, `status`, `inject` |
| `thread` | `CONFIG_AKIRA_THREAD` | `info`, `start`, `stop` |
| `ota` | `CONFIG_AKIRA_OTA` | `status`, `confirm` |
| `se050` | `CONFIG_AKIRA_SE050` | `info`, `rand`, `apdu`, `ecctest` |
| `http_start`, `http_status` | `CONFIG_AKIRA_HTTP_SERVER` | flat commands |

Zephyr's own commands (`log`, `kernel`, `device`, `flash`, `net`, …) are available as usual.

## Related Documentation

- [Troubleshooting Guide](../getting-started/troubleshooting.md)
- [Native Simulation](../platform/native-sim.md)

---

*Last updated: 2026-09-14 (AkiraOS v1.6.5)*
