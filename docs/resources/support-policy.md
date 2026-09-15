---
layout: default
title: Support & Compatibility
parent: Resources
nav_order: 6
permalink: /resources/support-policy
---

# Support and compatibility

Which AkiraOS release a product pins, how long it is supported, and which
firmware, WASM ABI and SDK versions work together.

## Release channels

| Channel | Branch | For | Support |
|---|---|---|---|
| Current | `v1.6.x` | active development; products track it during bring-up | until the next minor supersedes it |
| Maintenance | `v1.5.x` | the previous production line | critical + security fixes only |
| Deprecated | `v1.4.x` and older, `v1.3.x-deprecated` | — | none |

A **product pins an AkiraOS tag** in its `west.yml` (`revision: v1.6.4`), never a
moving branch, so its build is reproducible. Upgrade deliberately: bump the tag,
`west update`, rebuild, re-test, read the [CHANGELOG](https://github.com/ArturR0k3r/AkiraOS/blob/main/CHANGELOG.md)
for `!`-marked breaking changes.

### LTS

An **LTS** tag (planned from the first `v1.x` stable minor) receives security and
critical fixes for **24 months** from release; two LTS lines overlap so a product
always has ~12 months to migrate. Non-LTS minors are supported only until the
next minor. Until the first LTS is cut, treat `v1.6.x` as current-only.

## Version relationships

Three versions move at their own pace and must stay compatible:

- **AkiraOS firmware version** (`VERSION`, e.g. 1.6.4) — the module release a
  product pins.
- **WASM ABI version** (`include/akira_abi.h`, e.g. 1.0) — the native import
  interface apps are built against. Bumps **major** on a breaking import change,
  **minor** on an addition. See the
  [API stability policy](../api-stability-policy.md).
- **AkiraSDK release** — what apps are built with; it targets a WASM ABI range
  and is released on its own schedule (independently of the firmware).

### Compatibility matrix

| AkiraOS firmware | WASM ABI it provides | AkiraSDK releases that build compatible apps |
|---|---|---|
| 1.6.x | 1.0 | SDK releases declaring ABI 1.x (`abi: "1.0"`) |
| < 1.6.0 | none (pre-ABI) | apps with no `abi` key; loaded as legacy 1.x |

How it is enforced at runtime:

- An app's manifest `abi` **major** must equal the firmware's. A newer **minor**
  loads with a warning (calls to natives the firmware lacks trap). A missing
  `abi` key is treated as legacy 1.x.
- `min_akiraos_version` in the manifest is checked at install and load.
- CI (`scripts/check_wasm_abi.py`) fails if the firmware's exported natives and
  the SDK header drift outside the recorded allowlist, so the two repos cannot
  disagree silently.

## Reporting issues

File AkiraOS issues at the [GitHub repository](https://github.com/ArturR0k3r/AkiraOS/issues);
include the pinned AkiraOS tag, the WASM ABI version, the board, and the SDK
release of any app involved.
