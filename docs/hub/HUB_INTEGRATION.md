---
layout: default
title: AkiraHub Integration
parent: Architecture
nav_order: 11
---

# AkiraHub Integration

The device-to-hub protocol is **AkiraHub protocol v1**. It is specified and
implemented by AkiraPlatform's fleet client (`docs/hub-protocol-v1.md` in the
AkiraPlatform repository), and it replaces the draft that used to live on this page.

What AkiraOS provides for any fleet client, AkiraPlatform's or your own:

| Need | AkiraOS public API |
|---|---|
| Install, start, stop, uninstall apps; app-defined commands | `include/akira_apps.h` (installs go through app admission) |
| Write and stage firmware; confirm or roll back | `include/akira_ota.h`, `include/akira_boot_guard.h` |
| Know when the network comes up; app and OTA events | `include/akira_hooks.h` (`NET_UP`, `APP_*`, `OTA_*`) |
| Report security events | `include/akira_audit.h` (audit sinks) |
| Device identity in status reports | `include/akira_identity.h` |

The legacy in-tree cloud client (`CONFIG_AKIRA_CLOUD_CLIENT`) is deprecated in 1.7
and will be removed in 1.9. It never had a working transport.
