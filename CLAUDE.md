# CLAUDE.md

Guidance for Claude Code (and other agents) working in this repository.
Everything here must be followable from a fresh checkout; machine-specific
notes are in the last section and belong in `CLAUDE.local.md`.

## What AkiraOS is

AkiraOS is a secure embedded OS built on **Zephyr RTOS** that runs sandboxed
**WebAssembly (WASM)** applications via WAMR. Flagship target is ESP32-S3; it also
builds for ESP32 classic/C3/C6/H2, STM32 (U5/H7/L4), Nordic nRF54L15, and
RP2040/RP2350. Apps are isolated by a 64-bit **capability mask**: each app declares
the capabilities it needs in a manifest and the runtime denies anything ungranted.

AkiraOS is packaged as a **Zephyr module** (`zephyr/module.yml`): libraries build
from `zephyr/CMakeLists.txt`, Kconfig lives in `zephyr/Kconfig` behind
`menuconfig AKIRA_OS`, and the root app is just `src/main.c` → `akira_start()`.
Products build the same way from their own repos (see `templates/akira-product/`).

Firmware is **Apache-2.0**. CI (`.github/workflows/ci.yml`) fails on any
`GPL`/`AGPL`/`LGPL` SPDX identifier under `src/` or `include/` — relicense headers
when porting files from older branches.

## Repository layout

- `src/` — firmware C sources (drivers, runtime, api, connectivity, console_shell, …)
- `include/` — public headers (`akira_native_api.h`, `akira_capability.h`, `akira_abi.h`, …)
- `boards/` — per-board `.conf`/`.overlay` extras and MCUboot overrides (`<board>.mcuboot.conf`)
- `snippets/` — Zephyr snippets: `akira-board/boards/*` (hardware), `akira-reference`
  (full service selection), `akira-profile-*` (minimal/connected/console/sensor-node),
  `akira-display-*`, `akira-release`
- `zephyr/` — module glue (`module.yml`, `Kconfig`, `CMakeLists.txt`)
- `AkiraSDK/` — **git submodule**: WASM app SDK, sample apps, app toolchain
- `tools/`, `scripts/`, `cmake/` — build helpers, `west akira` commands, release checks
- `docs/` — Jekyll `just-the-docs` site (see "Docs site")
- `tests/`, `bench/`, `samples/` — twister tests, benchmarks, out-of-tree sample product

## Setup

- The repo lives inside a **west workspace**; build directories are created in the
  workspace root (the parent of this repo), not inside it.
- `west.yml` pins upstream Zephyr `v4.3.0`.
- Initialise the SDK submodule before touching WASM apps:
  `git submodule update --init --recursive`
- Firmware must build **without** the submodule; the SDK is a separate repo.

## Building firmware — `./build.sh`

`./build.sh` is the single entry point. `./build.sh -h` prints the live board list
(auto-discovered from `snippets/akira-board/boards/*.conf`) and all options.

Key options (`v1.6.x` interface):

| Option | Effect |
|---|---|
| `-b <board>` | Target board (default `native_sim`). Aliases accepted: `akiraconsole`, `akiraconsole_prod`, `esp32s3_super_mini`, `pico`, `stm32h7`, … |
| `-bl y` / `-bl o` | Build MCUboot **with** the app / bootloader only |
| `-r a` / `-r b` / `-r all` | Flash app / bootloader / both after build |
| `-e` | Erase flash before flashing |
| `-s` | Generate a CycloneDX 1.4 SBOM at `build-<board>/sbom.cdx.json` |
| `-k <key.pem>` | Sign the app with this product key; with `-bl`, MCUboot is built to verify it and refuse downgrades |
| `--release` | Apply the `akira-release` snippet and **fail** on dev-only security settings (dev MCUboot key, unsigned images, downgrade prevention off) |
| `--platform` | Enable AkiraPlatform enterprise modules |
| `-p <port>` / `--baud <rate>` | Serial port / baud for flashing |
| `-c` / `--full-clean` | Clean this board's build dirs / remove all `build*` dirs |

Examples:

```bash
./build.sh                                     # build + run native_sim
./build.sh -b akiraconsole_prod                # app only, AkiraConsole Prod
./build.sh -b akiraconsole_prod -bl y -r all   # MCUboot + app, flash both
./build.sh -b akiraconsole_prod -r a           # flash app only (builds if needed)
./build.sh -b esp32s3_super_mini
./build.sh -b <board> -bl y -k keys/mcuboot-signing.pem --release   # release build
./build.sh -c -b akiraconsole_prod             # clean one board
./build.sh --full-clean
```

Build dirs: `<workspace>/build-<board>` and `<workspace>/build-mcuboot-<board>`
(underscores in the board name become hyphens). ESP32 flash map: MCUboot at `0x0`
(`0x1000` on classic ESP32), app at `0x20000`.

Flashing:

- ESP32 → `esptool`; Nordic/STM32 → `west flash`; RP2040/RP2350 → a UF2 you copy to
  the `RPI-RP2` mass-storage drive (no auto-flash).
- **AkiraConsole Prod has no auto-download-reset** (GPIO46 strap). Start the flash,
  then during "Connecting…" **hold BOOT + tap RESET** and keep BOOT held until
  "Writing at…". Console: 115200 baud on `/dev/ttyUSB0` (CP2102N).

## Building WASM apps — `AkiraSDK/wasm_apps/build.sh`

Rebuilt app binaries show up as **submodule changes**; commit them in the SDK repo,
not here.

From `AkiraSDK/wasm_apps/`:

```bash
./build.sh                 # build all apps (C, Rust, Python) → bin/
./build.sh list
./build.sh <app_name>      # one C app (searches generic/ console_apps/ retro_games/)
./build.sh rust/<name>
./build.sh python/<name>
./build.sh clean
./build.sh aot [target]    # AOT-compile all bin/*.wasm → .aot
./build.sh <app> --aot[=target]
```

A `Makefile` wraps the same steps (`make`, `make check`, `make aot`,
`make install` to an SD card). `make check` verifies the toolchain.

Toolchains (env vars):

- C: **WASI SDK** at `/opt/wasi-sdk` (override with `WASI_SDK=`)
- Rust: `cargo` + `rustup target add wasm32-unknown-unknown`
- Python: `MICROPYTHON_WASM=/path/to/micropython.wasm`
- AOT: `wamrc` — build once from `modules/wasm-micro-runtime/wamr-compiler` (uses the
  bundled LLVM), then put it on `PATH` or set `WAMRC=`

AOT targets (`.aot` runs 10–50× faster than the interpreter and is the real deployment
path): `xtensa`/`esp32s3`, `thumb`/`nrf54l15` (Cortex-M33), `thumbv7em`/`stm32`
(Cortex-M7), `riscv32`/`esp32c3`, `x86_64`/`native`.

Manifest and capabilities: each app ships `manifest.json` (name, declared capabilities,
`abi`, `min_akiraos_version`). `scripts/embed_manifest.py` embeds it as the
`.akira.manifest` WASM custom section. `wamrc` **must** get
`--emit-custom-sections=.akira.manifest`, otherwise the runtime sees `cap_mask=0` and
denies every capability. The SDK `build.sh` already passes it; add it if you call
`wamrc` by hand.

Retro ROM games use a separate pipeline: `python3 tools/rom_to_aot.py <rom> -o out.aot`
(bundles the ROM into the binary). There is no rebuild-all; run it per ROM.

Installing apps on device: embed in firmware, copy to SD `apps/`, or install from
LittleFS via the shell (`app install <name|/fullpath> lfs`). `.akpkg` packaging is a
separate pipeline: `AkiraSDK/tools/akira-cli pack <wasm> <manifest>`.

## Running and testing

- **native_sim**: `./build.sh` builds and runs it; UART is the exe's stdin/stdout.
- **Unit tests**: `west twister -T tests/ -p native_sim`. Last known baseline:
  ~85 pass / 7 skip. The tree has pre-existing WAMR/AkiraOS warnings, so twister with
  `-Werror` fails; pass `--disable-warnings-as-errors` (the out-of-tree sample CI step
  does this).
- **OS shell on native_sim** needs an `sw0` devicetree alias plus
  `CONFIG_AKIRA_INPUT_API=y` and `CONFIG_INPUT_SHELL=y`. Frames are written to
  `/tmp/akira_framebuffer` as RGB565 little-endian.

## Hard-won conventions (read before non-trivial changes)

- **DRAM is the hard limit on ESP32-S3**, not flash or PSRAM. `akiraconsole_prod` and
  `esp32s3_super_mini` sit at ~99% internal DRAM with BT + WiFi + WASM enabled. New
  code must go to flash/IRAM, not static DRAM BSS. Move bulk statics to PSRAM with the
  `AKIRA_BULK_BSS` attribute (placed **after** the variable name). BT/WiFi controller
  buffers and WASM thread stacks **must stay in internal SRAM** (DMA and SPI0 flash
  sharing).
- **BLE on ESP32-S3** initialises entirely from the Zephyr system heap. Too small →
  `-ENOMEM` at init or `BLE assert emi.c` at enable. Diagnose peak usage with
  `kernel heap` in the shell.
- **Capabilities**: core bits 0–47 (`AKIRA_CORE_CAPABILITY`), vendor bits 48–63
  (`AKIRA_CAPABILITY_DEFINE`). Native APIs register with `AKIRA_NATIVE_API_DEFINE`,
  hooks with `AKIRA_HOOK_DEFINE`. Product/vendor subsystems must compile into `app`:
  a late-registered `zephyr_library()` misses whole-archive capture and its
  iterable-section entries are dropped silently.
- **Branches matter.** Day-to-day work is on `v1.6.x`; PRs target `main`. Some features
  exist only on other branches and were **deliberately removed** elsewhere — e.g. the
  native input API and WASM system shell live on `C0ns0l3-Firmware` and are not on
  `v1.6.x` (use the LVGL input path, `CONFIG_AKIRA_LVGL_INPUT_ZEPHYR`, there).
  KeyaPlatform keyboard work is on `KeyaPlatform`. Never "restore" branch-specific
  code across branches.
- **Board ground truth is the KiCad netlist**, not the board files. When a pin map or
  part is in doubt, parse the `.kicad_pcb` for that hardware revision — board files
  have been wrong before.
- RISC-V ESP32 targets (C3/C6/H2) are verified in **CI**; use native_sim, the
  ESP32-S3 boards, and `nrf54l15dk` for local checks.

## Docs site (`docs/`)

Published at **docs.akiraos.dev** via GitHub Pages (`just-the-docs`, served from
`docs/`).

- Relative links that escape `docs/` 404 on the live site. Link to repo files with
  absolute URLs: `https://github.com/ArturR0k3r/AkiraOS/blob/main/<path>`.
- Every page needs explicit front matter. Do **not** add `parent:` through
  `_config.yml` `defaults` path scopes — it corrupts the nav tree.
- Regenerate API content from the authoritative sources instead of hand-editing:
  `src/api/akira_export_api.c` (WASM natives), `src/runtime/security.h` (capability
  bits), `boards/*.conf` (board metadata), and `zephyr/Kconfig` (config options).

## Git and workflow

- Work on `v1.6.x`; PRs go to `main`. Branch first if you are on a protected branch.
- Commit and push **only when asked**.
- Conventional commits (`feat`, `fix`, `fix(scope)!`, …) with a `CHANGELOG` entry.
- The root `VERSION` file owns the version; Zephyr parses it into `APP_VERSION_*`.
  Don't hard-code version strings elsewhere.

## Local environment notes (move to `CLAUDE.local.md`)

These describe Artur's workstation, not the repo, and will not hold on other machines
or in CI.

- Local Zephyr is a PenEngineering fork at `~/Akira/zephyr` (`v4.3.0-26`), not the
  upstream `v4.3.0` that `west.yml` pins. Memory numbers from local builds differ
  from CI.
- RISC-V ESP32 (C3/C6/H2) builds fail locally on `zsr.h`; rely on CI for those.
- KiCad sources for the boards are under `~/Akira/HW*/`.