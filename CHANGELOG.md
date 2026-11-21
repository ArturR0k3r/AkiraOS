# Changelog

All notable changes to AkiraOS are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Fixed
- `fix(akiraconsole_prod)`: The reference configuration no longer builds the HTTP
  management server: the board has no Wi-Fi there, so the server could never bind,
  but its 6 KB thread stack and buffers sat in internal DRAM. Frees about 9 KB, which
  AkiraPlatform builds for this board need (they overflowed `dram0_0_seg` by 1.3 KB on
  upstream Zephyr after the 1.7.1 security fixes). Turn `CONFIG_AKIRA_HTTP_SERVER` back
  on together with `CONFIG_WIFI`.
- `ci(release)`: The release workflow never built: the Zephyr CI container has no
  registered Zephyr SDK, so every board failed at CMake (`Could not find ...
  "Zephyr-sdk"`, also in the 1.6.5–1.6.7 runs). It now installs SDK 0.17.4 with the
  board's toolchain, both checked against the published SHA-256 sums.

---

## [1.7.1] — Unreleased

### Security
Fixes from a STRIDE review of AkiraOS and AkiraPlatform (2026-11-21). Several change
behaviour; the breaking ones are marked **(breaking)**.
- `fix(bt)!`: The BLE companion service accepted commands from any unpaired device in radio
  range, including `shell.exec`, file read/write anywhere on `/lfs`, settings read
  (decrypted Wi-Fi PSK) and app install. Its characteristics, and BLE app transfer's, now
  need an authenticated LE Secure Connections bond (`CONFIG_AKIRA_BT_SECURE_PAIRING`,
  default y): outside HID mode the device pairs as DisplayOnly and raises the new
  `AKIRA_HOOK_BT_PASSKEY_DISPLAY` / `AKIRA_HOOK_BT_PAIRING_DONE` hooks so the product can
  show the passkey; Just Works pairing is refused there. HID mode keeps Just Works.
  **(breaking)**: phones must re-pair with the passkey; products must show it.
- `fix(bt)`: Companion `shell.exec` is behind `CONFIG_AKIRA_BT_COMPANION_SHELL` (off in
  release builds). File operations are limited to `/lfs` and `/SD:` without `..`;
  `/lfs/akp` is never readable and app storage (`/lfs/apps`, `/SD:/apps`) is never
  writable over BLE. App names are validated before they name a staging file. Transfer
  state is locked between the BT RX thread and the work queue (use-after-free on
  disconnect), JSON builders no longer write past their buffers on truncation, file
  writes are flushed off the BT thread, and advertising restarts after a disconnect.
- `fix(bt)`: BLE app transfer frees its staging buffer after an install, drops a transfer
  when its peer disconnects, reports a failed start, and never blocks the BT RX thread.
  The echo service's hex dump no longer overflows its stack buffer.
- `fix(settings)`: Encrypted settings never leave the device: `settings get` / `list` and the
  BLE companion show `[ENCRYPTED]`. New `akira_settings_get_exportable()`.
- `fix(release)!`: `akira-release` turns off `DEVMEM_SHELL` (Zephyr enables it whenever the
  shell is full-size), `I2C_SHELL`, `GPIO_SHELL`, `FLASH_SHELL`, `FILE_SYSTEM_SHELL` and
  `AKIRA_OTA_BLE`, and the release checks refuse them, `AKIRA_BT_COMPANION_SHELL`, BLE
  management without secure pairing, and the published default
  `AKIRA_SETTINGS_ENCRYPTION_KEY_HEX` when no settings key provider is built in. A USB HID
  app handler is reported as an advisory. **(breaking)**: release builds need a product
  settings key (`AKIRA_SETTINGS_KEY_HEX` for `build.sh`) or a key provider.
- `fix(apps)!`: App names must be lowercase `[a-z0-9_.-]` (start with a letter or digit, no
  `..`, no trailing dot, `akira_app_name_valid()`): a name such as `x/../002_victim` or
  `acme/../../../akp/policy` wrote outside `/lfs/apps`, and FAT case folding or trailing
  dots let two apps share a data folder. Install, streaming install, SD-XIP and the data
  jail all apply the rule. **(breaking)** for apps with other names.
- `fix(apps)`: Updating an attested app requires the same signer. Uninstalling an app
  deletes its data folder. `app.control` and `ble.spam` join the privileged capabilities
  hardened builds withhold from unattested apps.
- `fix(ipc)`: `<app>.cmd` / `<app>.evt` belong to `<app>`: no other app can subscribe to or
  publish on them, so an app can no longer read the commands the companion or a fleet hub
  sends another app, or forge its replies. Apps cannot publish on `akira.*`.
- `fix(runtime)`: The WASM and AOT custom-section scanners (manifest and integrity checks,
  which run before admission) used `pos + size > total`, which wraps on 32-bit targets: a
  crafted section size made the installer loop forever or read past the buffer. The tar
  header name in `.akpkg` is bounded, and packages shorter than a gzip header are refused.
- `fix(admission)`: INSTALL and LOAD now pass providers the same manifest hash (none when the
  binary embeds `.akira.manifest`) and the manifest version at install is no longer lost;
  signed packages with an embedded manifest were refused at load as "tampered". The
  context gains `is_aot`.
- `fix(ota)`: `ota_start_update()` refuses while an update is staged or the running image is
  on trial (erasing the slot would destroy the rollback image) and claims the OTA state
  atomically. New `akira_ota_finish_ex(AKIRA_OTA_FINISH_REQUIRE_NEWER)`; app-driven OTA
  requires the staged MCUboot header to be newer instead of trusting the manifest's
  version. Apps can no longer roll back during a trial boot or when downgrades are refused.
  Firmware URLs with userinfo are refused (egress-policy bypass). `AKIRA_OTA_BLE` now
  defaults to n.
- `fix(mesh)!`: An RREP for a destination without a session no longer sends the mesh into an
  endless re-queue/RREQ loop. DATA MACs now cover the header (all but the TTL) and are
  checked against a per-session replay window, so frames can't be re-sent under a new
  sequence number or reflected to their sender. **(breaking)**: wire version 2, not
  compatible with version 1 nodes. Mesh identity keys are still not authenticated (see the
  security docs). `mesh_init` no longer tears down another app's radio session, and
  `mesh_get_nodes` / `mesh_get_stats` check the caller owns the mesh.
- `fix(aiinfer)`: Models whose tensors don't match their buffers (declared shape larger than
  the buffer, operator outputs or graph inputs backed by model data, unknown types) are
  refused, and input/output tensors must lie in the arena: a crafted model let
  `aiinfer_run()` copy past the model buffer into host memory.
- `fix(http)`: State-changing routes refuse requests whose `Origin` the CORS policy doesn't
  allow (cross-site and DNS-rebinding requests from a browser); `*.akiraos.dev` origins
  must be https. The multipart parser searches only received bytes (out-of-bounds read),
  connections time out, and a refused transfer start no longer leaves the transport busy.
- `fix(se050)!`: `se050_object_exists()` returns 1 / 0 / negative errno instead of `bool`: a
  failed exchange used to read as "absent", which let key generation overwrite a
  provisioned object. The T=1' receive loop rejects empty chained blocks and bounds the
  chain (a bus device could hang boot).
- `fix(build)`: `build.sh` rebuilds before flashing when `--release` or `-k` is given (an
  existing development build was flashed silently), checks the MCUboot it flashes, and
  detects published development keys by public key, not file name. The release workflow
  builds `akiraconsole_prod` with `--release`, fails instead of shipping unsigned images,
  keeps the signing key out of steps that run third-party code, publishes from a separate
  job, and pins third-party actions. `west akira release` gains `--release` and `-D`.
- `fix(settings)`: SD-mode key parsing bounds the namespace copy; the crash record's app
  name is terminated before it is printed.

---

## [1.7.0] — Not tagged (ships as part of 1.7.1)

### Added
- `feat(security)`: App admission (`include/akira_admission.h`). Providers registered with
  `AKIRA_APP_ADMISSION_DEFINE()` see every install (HTTP, BLE, USB, SD, shell, fleet) with
  the binary hash, package digest and signatures, and every load (including SD-XIP and cloud
  loads). They can refuse an app, attest it, or clamp its capabilities. The install verdict
  is kept in an admission record bound to the binary and stored manifest by SHA-256; a load
  whose bytes changed is refused. `attested` is now a per-app result that drives
  `akira_capability_grant()` and the AOT rule, replacing the compile-time flag.
- `feat(security)`: Baseline app signature check. `CONFIG_AKIRA_APP_PUBKEY` (Ed25519 public
  key) attests `.akpkg` packages whose `sig.ed25519` (written by `akira-cli sign`) verifies.
  With `CONFIG_AKIRA_REQUIRE_SIGNED_APPS=y`, apps nobody attested are refused at install and
  load. Ed25519 verification (`include/akira_ed25519.h`) is the fiat-crypto verifier from
  MCUboot (MIT), since mbedTLS 3 has no EdDSA.
- `feat(security)`: Network egress policy providers (`include/akira_net_policy.h`) gate WASM
  `net_connect`, `net_bind`, `net_listen` and the URLs `ota_check` / `ota_fetch_and_apply`
  fetch; the manifest's `network_policy` reaches providers through admission.
- `feat(security)`: Settings key providers (`include/akira_keyprovider.h`), audit sinks and
  `akira_audit_emit()` (`include/akira_audit.h`, sequence-numbered, delivered outside the
  audit lock with drop accounting), and identity providers for the provisioned serial,
  organisation and certificate fingerprint (`include/akira_identity.h`, shown by
  `akira version` and `GET /api/v1/info`).
- `feat(api)`: Public façades for product firmware and device-management modules:
  `akira_os.h` (`akira_start()`), `akira_apps.h`, `akira_ota.h`, `akira_wifi.h`,
  `akira_mem.h`. `AKIRA_CHECK_CAP_OR_RETURN()` and friends move to `akira_capability.h`.
  `akira_apps_send_command()` and `akira_apps_get_commands()` expose the app command
  bridge (`<name>.cmd` / `<name>.evt`) and the manifest `commands` array.
- `feat(hooks)`: The ten declared-but-silent events are now raised: `NET_UP` / `NET_DOWN`,
  `BT_CONNECTED` / `BT_DISCONNECTED`, `USB_CONFIGURED` / `USB_DISCONNECTED`, `OTA_STARTED`,
  `OTA_STAGED`, `OTA_PRE_REBOOT`, `APP_FAILED`; `OTA_ERROR` also covers download, hash and
  downgrade failures. App events carry `registry_id`.
- `docs(api)`: `docs/api-reference/extension-points.md`.
- `feat(drivers)`: Microchip ATECC608B driver upstreamed from the Latch product firmware
  (`include/drivers/secure_element/atecc608b.h`, binding `microchip,atecc608b`) and extended
  with on-chip P-256 key generation, public key, sign, HMAC-SHA256 with a slot key, data-slot
  writes, config-zone write and zone/slot locks. The new commands are not yet validated on
  hardware.
- `feat(drivers)`: The SE050 header is public (`include/drivers/secure_element/se050.h`) and
  gains HMAC key storage, on-chip HMAC-SHA256 and object deletion (not yet validated on
  hardware). `se050 apdu` / `se050 ecctest` now need `CONFIG_AKIRA_SHELL_DEBUG_COMMANDS`.
- `feat(bench)`: Added `bench/exec_speed` — a WebAssembly execution-speed benchmark that runs
  one identical compute kernel three ways (native C, WAMR classic interpreter, and `wamrc` AOT)
  and times each with `CCOUNT`, asserting all three results match. Measured on ESP32-S3 at
  240 MHz: the interpreter is **71.8× slower than native C**, AOT is **78.3× faster than the
  interpreter**, and **AOT runs at native speed** (0.92× native cycles — AOT is LLVM `-O3`, the
  native build GCC `-Os`). Raw data in `bench/exec_speed/results-akiraconsole_prod.json`.
- `fix(wamr)`: Fixed ESP32-S3 on-device AOT loading, which was unreliable (worked only by cache
  luck). The WAMR Zephyr platform's `os_dcache_flush()` used `sys_cache_*`, which are no-ops on
  this SoC (no `CONFIG_CACHE` driver), so after the loader wrote `.aot` code through the PSRAM
  data-bus mirror its instruction-bus reads of section metadata returned stale cache → corrupt
  code pointer → "target address out of range". The fix uses the ESP ROM primitives
  (`Cache_WriteBack_All` + `Cache_Invalidate_ICache_All`). Carried in
  `bench/exec_speed/wamr_patch/zephyr_platform.c`; should be upstreamed into the WAMR module so
  AOT (including the retro cores) is reliable.
- `docs(perf)`: Published the measured interpreter / AOT / native execution speeds on ESP32-S3,
  superseding the withdrawn "AOT 10–50× faster" / "~90–95 % of native" estimates.
- `docs(flash)`: Added a browser flasher at `docs.akiraos.dev/flash` (ESP Web Tools over
  Web Serial) with a board picker and Wi-Fi provisioning notes, plus per-board manifests.
- `ci(release)`: For the ESP32-S3 boards, also build a merged full-flash image
  (`akiraos-<board>-flash.bin`, MCUboot + app) that the browser flasher consumes; best
  effort, so it never blocks the rest of the release.
- `docs(readme)`: Rewrote the README around the launch story — one-liner, browser/catalogue/
  flash call-to-action, a 60-second start, C and Rust examples, "Why AkiraOS", an honest
  comparison table (vs MicroPython / Toit / Ocre / Arduino-ESP-IDF), a current hardening
  block, and an open-source-vs-commercial section. Trimmed to under 250 lines.
- `docs`: Added `ROADMAP.md` (Now / Next / Later).
- `docs(contributing)`: Added a "Publish your app to the AkiraConsole App Store" guide and
  Discord/Matrix chat placeholders.
- `docs(launch)`: Added `docs/launch/` announcement drafts (Show HN, r/embedded, r/esp32,
  Hackaday, Zephyr newsletter), excluded from the published docs site.
- `ci(release)`: On a version tag, build prebuilt firmware for `akiraconsole`,
  `akiraconsole_prod`, `esp32s3_devkitm`, `nrf54l15dk` and `rpi_pico2` and attach
  them to the GitHub release. Artifacts are named `akiraos-<tag>-<board>.<ext>`
  (`.bin`/`.signed.bin`, `.uf2` for Pico, `.hex`, `.elf`, a per-board SBOM and
  `SHA256SUMS`). ESP boards and the nRF/Pico boards no longer collide on one
  `zephyr.bin`. `west akira release` now also collects `.uf2` and `.hex`.
- `feat(runtime)!`: No WASM app can crash or hang the system
  ([App Containment](docs/architecture/app-containment.md)).
  - A CPU fault or `k_oops()` in an app's thread, including inside a native, stops only that
    app; the slot is reclaimed and the app is reported crashed. A fault inside a lock-holding
    native parks the slot until reboot instead of running cleanup that could block on the
    lost lock. Stack overflows are contained only with a hardware stack guard. Every fatal
    handler (default, `AKIRA_PANIC`, boot guard) asks the runtime first.
  - Stopping an app wakes every wait it can be in, aborts a thread still spinning in WASM
    after 1.5 s, and never aborts one inside a lock-holding native critical section (the stop
    returns `-EBUSY` instead). An aborted app's slot is freed (it used to leak a runtime slot
    per abort), and a stop never touches a slot that was reused while it waited.
  - `include/akira_app_containment.h`: `AKIRA_APP_RESOURCE_DEFINE()` release hooks run on every
    exit path before the app's memory is freed; `akira_runtime_sleep()`,
    `akira_native_critical_enter/exit()` and `akira_runtime_stop_requested()` for natives,
    product natives included.
  - WAMR memory allocated by a running app is capped (`CONFIG_AKIRA_WASM_APP_MEM_LIMIT_KB`) and,
    with PSRAM, never falls back to internal SRAM.
  - WAMR validates every module (`CONFIG_AKIRA_WAMR_MINI_LOADER` now defaults to `n`); 3 KB
    native stack guard (`CONFIG_AKIRA_WASM_STACK_GUARD_SIZE`).
  - Every native API audited: full-length pointer checks, bounded and stop-aware waits,
    per-app ownership of streams, files, timers, pins, radios, BLE, HID, alarms, model handles
    and subscriptions, released when the app ends; rate-limited app output and warnings.
  - `CONFIG_AKIRA_SANDBOX_SELFTEST` adds `selftest_fault()` to test it (refused in release
    builds).
- `feat(api)`: `crypto_ed25519_keygen()` and `crypto_ed25519_sign()` work
  (`CONFIG_AKIRA_WASM_CRYPTO_ED25519`). They used PSA, which mbedTLS doesn't implement for
  EdDSA, so they could only fail; signing now runs on the in-tree Ed25519 code
  (`src/lib/crypto/ed25519/ed25519_sign.c`, checked against RFC 8032 vectors).

---


### Changed
- `feat(security)!`: `akira_capability_grant(requested, attested, ...)`: the second argument
  is the admission verdict, no longer "manifest embedded in the binary". A signed package's
  JSON manifest is covered by its signature and by the admission record, so signed apps no
  longer need an embedded manifest to be attested.
- `fix(ota)`: `ota_manager_init()` runs whenever OTA is built (MCUboot + flash map), not only
  with the HTTP server, so release builds (`HTTP_SERVER=n`) can update.
- `feat(api)!`: Fixed rules for app access to shared hardware and data, whatever the manifest
  asks for:
  - GPIO: `CONFIG_AKIRA_WASM_GPIO_APP_PINS` minus every devicetree, chip-reserved and strapping
    pin; an app reads and writes only pins it configured. AkiraConsole Prod: GPIO39/40.
  - ADC: `CONFIG_AKIRA_WASM_ADC_CHANNEL_MASK`, system pads refused. AkiraConsole Prod's app ADC
    moves to SAR ADC1 channel 1 (the pot): SAR ADC2's channels are the SPI2 and SD pins.
  - I2C: system device addresses refused. UART: system UARTs refused, writes bounded. PWM:
    only channels no system node uses. `wdt_pet()` no longer feeds the hardware watchdog.
  - Files: 2 open per app, per-file quota, a quarter of the volume kept free, 4 KB chunked I/O.
  - Settings: each app has a private namespace (keys saved under the old global names are no
    longer visible to apps); OS keys are out of reach.
  - Power: apps can't select light sleep, deep sleep or hibernate. OTA: apps can't confirm an
    image.
  - `app_start()`, `app_stop()`, `app_install_from_sd()` are queued and rate-limited.
- `fix(api)!`: Native signatures that didn't match their C functions: `app_check_update`
  `(*~)i`; `matter_commission`, `matter_send`, `matter_subscribe` and `matter_poll` now match
  the SDK.
- `fix(log)`: AkiraConsole Prod logs in deferred mode, so a thread aborted while logging holds
  no console lock.
- `feat(security)!`: Unattested AOT apps load in every build, release included, with a
  warning and an audit record: running one is the user's choice and risk.
  `CONFIG_AKIRA_HARDEN_UNATTESTED_APPS` now only withholds privileged capabilities; the new
  `CONFIG_AKIRA_REFUSE_UNATTESTED_AOT` (default `n`) refuses unattested AOT.
- `chore(version)`: 1.7.0.

### Deprecated
- `CONFIG_AKIRA_CLOUD_CLIENT` (`src/connectivity/cloud`) now defaults to `n` and selects
  `DEPRECATED`; removal in 1.9. It never had a working transport. The BLE companion's
  `apps.update` replies "update not supported" without it.

### Removed
- The header-less weak hooks `akira_settings_get_encryption_key()`,
  `akira_net_policy_check(app_id, host, port)`, `akira_platform_allowlist_verify()` and
  `akira_platform_pqc_verify()`, replaced by the provider APIs above.
- `CONFIG_AKIRA_AUDIT_LOG_HMAC` / `CONFIG_AKIRA_PLATFORM_AUDIT_HMAC` and the per-entry
  `hmac` field: core no longer includes a commercial header or signs audit records under a
  spinlock; sign in an audit sink instead. The akiraclaw bench `prj_hmac.conf` variant goes
  with it.

### Fixed
- `fix(api)`: The WASM crypto API (`CONFIG_AKIRA_WASM_CRYPTO`) didn't build: Zephyr no longer
  ships TinyCrypt. It runs on mbedTLS now; CBC and CTR keep TinyCrypt's exact behaviour (CTR
  increments the last 4 bytes of the counter block), so existing ciphertexts still decrypt.
  `CONFIG_AKIRA_SYNC` no longer selects TinyCrypt either.
- `fix(api)`: Every `fs_*` call failed: the jail `/akira/apps/<app>/` is on no mounted
  filesystem. Apps now get the storage API's directory, `/SD:/apps/<app>/` with a card or
  `/lfs/apps/<app>/` without, created on the first file or directory they make.
- `fix(wdt)`: The hardware watchdog was never armed on any board: `akira_wdt` looked only
  for a `zephyr,watchdog` node, and real watchdogs have vendor compatibles. It now uses the
  board's `watchdog0` alias. A hang is now reset after `CONFIG_AKIRA_WDT_TIMEOUT_MS` (30 s
  by default); verified on AkiraConsole Prod.
- `fix(shell)`: Zephyr's app version shell (on by default once an application has a
  `VERSION` file) registered a second root `app` command and hid AkiraOS's
  `app install/start/stop/list`. AkiraOS now defaults `CONFIG_APP_VERSION_SHELL` to `n`.
- `fix(log)`: AkiraConsole Prod's deferred log buffer is 4 KB; 2 KB dropped about 24 boot
  messages.
- `fix(settings)`: every settings request freed its work item inside its own handler (a
  use-after-free); app GETs no longer have the settings thread write into app memory; secret
  values are no longer logged at boot.
- `fix(apps)`: installing, uninstalling or shutting down stopped apps while holding the
  registry lock that their exit callback needs, so a slot could be reused under a live thread;
  the runtime now also refuses to reuse a slot whose previous thread is still running.
- `fix(net)`: the network poll thread spun at full CPU once more streams were open than
  `CONFIG_ZVFS_POLL_MAX` allows.
- `fix(usb)`: disabling and re-enabling USB HID no longer leaves it dead until replug.
- `fix(http)`: `POST /api/v1/apps/start`, `/stop` and `DELETE /api/v1/apps` now require the
  upload token like install and firmware upload did.
- `fix(config)`: `CONFIG_AKIRA_APP_MAX_INSTALLED` range raised to 32 (24 in `prj.conf` broke
  configuration); the registry buffers moved off the stack.
- `fix(build)`: `CONFIG_AKIRA_OS_SHELL=y` without `src/console_shell/` now fails with a clear
  message; removed eight non-existent include directories from the `akira_os` target.
- `fix(sbom)`: `west akira sbom` names the repository that holds `west.yml` (the product)
  after its directory with its commit, instead of west's placeholder `manifest` at `HEAD`
  with URL `N/A`.

## [1.6.7] — 2026-11-07

> Documentation and developer-experience pass for the public launch: point the
> repo at the browser simulator and the app catalogue, and make every figure and
> claim either sourced or explicitly unmeasured. No firmware behaviour changed.

### Documentation
- `docs(readme)`: Added first-screen call-to-action links — Try in browser (the
  AkiraConsole simulator), Browse apps (the catalogue), Flash your board — and a
  commented-out `assets/simulator.gif` demo slot with a TODO.
- `docs(quickstart)`: Added Path 0 (no-install browser simulator) ahead of the
  host-simulator and hardware paths, and a pointer to the simulator from the
  app-upload section.
- `docs(perf)`: `docs/resources/performance.md` is now the single source for
  performance figures. The Capability Guard overhead ("~60 ns") was an estimate, not
  a measurement; every mention now links to the page, which states the methodology
  (`bench/akiraclaw_bench` B1, ESP32-S3 at 240 MHz, `CCOUNT`) and marks the figure
  as not yet measured.
- `docs(perf)`: Withdrew the unsourced WAMR speed figures ("AOT 10–50× faster than
  the interpreter", "~70 % / ~90 % of native", per-workload speed-ups). The docs now
  name the baseline — the WAMR **classic** interpreter, since the fast interpreter is
  not enabled — and mark both ratios (AOT vs interpreter, AOT vs native C) as not yet
  measured. Also fixed `CONFIG_WAMR_BUILD_AOT` → `CONFIG_WAMR_AOT_SUPPORT` and removed
  a reference to a nonexistent `akira_perf` shell command.
- `docs(sim)`: Removed a hard-coded `/home/<user>/…` path from
  `src/drivers/sim/README.md` and noted that the SDL2 module is not part of the
  v1.6.x build.
- `docs`: One language list everywhere, kept in
  `docs/development/building-apps.md#language-support`: C is supported; Rust and
  MicroPython are experimental (no `micropython.wasm` with the `_akira` module is
  shipped yet). C++, AssemblyScript and TinyGo were listed without bindings or
  samples and are now described as such.
- `docs(runtime)`: Explained the "8 installed · 2 running" limits: they are the
  Kconfig defaults of `CONFIG_AKIRA_APP_MAX_INSTALLED` (registry slots, static RAM)
  and `CONFIG_AKIRA_APP_MAX_RUNNING` (one WAMR instance and SRAM stack per app),
  bounded by `CONFIG_MAX_CONTAINERS`, with how to change them and why lowering the
  installed limit resets the registry.
- `docs`: Fixed every broken link in the root Markdown files and `docs/`: the
  CONTRIBUTING table of contents (emoji headings), API-reference anchors that pointed
  at sections living in the AkiraSDK reference, links into the `AkiraSDK` submodule
  (they 404 on GitHub), moved board `.conf` files, a moved Zephyr page, a wrong GitHub
  organisation and a wrong relative path. The README's first build command used a
  `build_wasm_app.sh` script that does not exist; it now uses `./build.sh hello_world`.

---

## [1.6.6] — 2026-11-02

### Fixed
- `fix(identity)`: `akira version` and `GET /api/v1/info` printed the literal
  `BUILD_VERSION` instead of the Zephyr version: `<zephyr/version.h>` was never
  included. Found on AkiraConsole Prod hardware; the shell had the same bug
  before 1.6.5.
- `fix(security)`: The boot log said `signing=enforced` although no in-tree code
  verifies app signatures. It now reports `required`
  (`CONFIG_AKIRA_REQUIRE_SIGNED_APPS`), `unattested apps unprivileged`
  (`CONFIG_AKIRA_HARDEN_UNATTESTED_APPS`) or `not verified`.

---

## [1.6.5] — 2026-11-02

> Development series on the `v1.6.x` branch, tagged `v1.6.5`. This section is
> backfilled from the commits since 1.5.8 plus the production-readiness
> hardening pass.

### Implemented (previously stubbed)
- `feat(sensors)`: BME280 register writes over I2C (`i2c_reg_write_byte`) — was `-ENOTSUP`.
- `feat(cloud)`: Real device telemetry in the status payload — battery via the power
  manager, free heap via `sys_heap_runtime_stats_get`, CPU% via Zephyr thread runtime
  stats — replacing hardcoded `3700 mV / 85% / 0 / 0`.
- `feat(http)`: Static file serving from the filesystem (`CONFIG_AKIRA_HTTP_STATIC_FILES`,
  default n) with extension→Content-Type mapping and path-traversal protection.
- `feat(ota)`: WASM app OTA — real HTTP(S) manifest fetch + firmware download streamed
  into the OTA manager with SHA-256 verification and anti-rollback (refuses non-newer
  versions); `-ENOSYS` fallback without `CONFIG_HTTP_CLIENT`.
- `feat(http)`: RFC 6455 WebSocket server — opening handshake, server→client text/binary
  frame encoding, client frame decode (mask, ping/pong/close) — replacing the unframed stub.
- `feat(coap)`: DNS resolution (hostnames), Content-Format option parsing, a fixed
  `coap_header_get_token()` contract, and RFC 7959 Block2 GET / Block1 PUT transfer.
- `feat(display)`: LVGL input driven by real Zephyr input events (gpio-keys / D-pad /
  gamepad / touch) via `INPUT_CALLBACK_DEFINE` (`CONFIG_AKIRA_LVGL_INPUT_ZEPHYR`).
- `feat(radio)`: 802.15.4 raw receive via net promiscuous mode with real per-frame
  LQI/RSSI (`CONFIG_AKIRA_RADIO_802154_PROMISC_RX`), replacing a hardcoded LQI of 200.

### Still stubbed (need more than firmware)
- USB-host mass-storage mount (needs an MSC host class + disk/FAT backend not vendored),
  Matter non-accessory path (needs the CHIP SDK), Thread (needs OpenThread), and mesh WASM
  app distribution (large RF-transport change) return `-ENOSYS`/stay gated until integrated.

### Security & hardening (production-readiness pass)
- `feat(ota)`: Verify a downloaded image's SHA-256 against authenticated metadata
  before it is ever marked bootable (`CONFIG_AKIRA_OTA_REQUIRE_HASH`,
  `ota_set_expected_sha256()`); the old 4-byte magic-only check is no longer
  sufficient.
- `feat(security)`: Clamp the capability mask an app can self-assert from its
  (unauthenticated) manifest — `"*"` is bounded to defined capabilities and
  unsigned apps are limited by `CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK`; privileged
  grants to unattested apps are audit-logged.
- `feat(settings)`: Derive a per-device AES-256 key from the hardware unique ID
  (`CONFIG_AKIRA_SETTINGS_PER_DEVICE_KEY`, fail-closed) instead of a shared
  compile-time key.
- `fix(license)`: Relicense first-party USB CDC serial, the FUSB302 VBUS-sense
  driver, and the BLE companion service from GPL-3.0 to Apache-2.0; add a CI
  SPDX/copyleft gate and a `NOTICE` file.
- `docs`: Correct README secure-boot/OTA claims to the real (in-progress) status.
- `ci`: Version-consistency gate, expanded board matrix, pinned container image.
- Advertised-but-incomplete entry points now return `-ENOSYS`/`-ENOTSUP` instead
  of reporting fake success (pending full implementation, see below).

### SESIP Level 2 evaluated configuration
- `feat(build)`: `build.sh -k <key.pem>` builds MCUboot to verify images signed
  with the product key (signature type from the key, overriding Espressif's
  `BOOT_SIGNATURE_TYPE_NONE`), with `MCUBOOT_DOWNGRADE_PREVENTION` and
  `BOOT_VALIDATE_SLOT0` (`boards/mcuboot-secure-update.conf`), and signs the app.
  `--release` applies the new `akira-release` snippet and fails if the built
  bootloader does not enforce signed, non-downgrading updates.
- `feat(ota)`: `CONFIG_AKIRA_OTA_REFUSE_DOWNGRADE` (on in release builds) refuses
  an older image before rebooting.
- `fix(ota)`: BLE OTA characteristics require an encrypted link, like BLE app
  transfer already did.
- `fix(security)`: Validate the full pointer range in native APIs that checked
  only the first byte or nothing: `app_list`, `app_get_self_name`,
  `msg_publish`, `msg_recv`, `wifi_scan_rssi`, `display_get_size`,
  `ha_light_poll` and the Python WASI `fd_*` shims. `sd_scan_wasm` treated its
  WAMR-validated native pointer as an app offset and never worked.
- `fix(security)`: The `fs.*` jail passed a module *instance* to
  `wasm_runtime_get_module_name()` (which takes a module) and fell back to a
  shared `unknown` directory. It now uses the runtime's app name, refuses names
  with `/` or `..`, and gives an unnamed caller no files.
- `feat(security)`: `CONFIG_AKIRA_HARDEN_UNATTESTED_APPS` (on in release builds):
  an app no signature verifier attested gets no privileged capability and cannot
  load AOT native code. `CONFIG_AKIRA_WAMR_MINI_LOADER` (off in release builds)
  makes WAMR's validation-skipping mini loader optional.
- `feat(security)`: Release checks also fail on unhardened unattested apps, the
  mini loader without signed apps, the telnet shell and `debug memdump`
  (`CONFIG_AKIRA_SHELL_DEBUG_COMMANDS`), and warn about the login-less serial shell.
- `feat(identity)`: `akira_identity_get()` reports every platform part and its
  version — AkiraOS version and git revision, the MCUboot-verified image version,
  Zephyr, WAMR and its revision, board, device ID — through `akira version` and
  `GET /api/v1/info`.
- `fix(boards)`: AkiraConsole Prod MCUboot builds again: the I/O-expander driver
  pulled in by the board overlay is turned off for the bootloader.
- `docs`: [SESIP evaluated configuration](docs/compliance/sesip-evaluated-configuration.md).

### Added
- `feat(security)`: Fail-closed app verification gate (`CONFIG_AKIRA_REQUIRE_SIGNED_APPS`).
- `feat(pqc)`: Dilithium-2 post-quantum signature verify path in the runtime.
- `feat(matter)`: Device-as-endpoint (accessory) support with an in-firmware mock co-processor for tests; accessory mock build enabled on esp32c6.
- `feat(mqtt)`: Home Assistant MQTT client integration (discovery + `ha_light_*`/`mqtt_*` WASM natives).
- `feat(mesh)`: AODV routing with reliable delivery and capability-based transport.
- `feat(radio)`: CC1121 OOK modulation, raw OOK capture/replay, continuous RX with frequency-offset registers; LR2021 LoRa modulation (runtime-selectable with FSK).
- `feat(api)`: HID modifier + FIDO/U2F Report ID 4 channel, AES-256-CTR, WiFi deauth, and raw RF capture/replay WASM APIs.
- `feat(board)`: esp32s3_super_mini — 2 MB Quad PSRAM + WiFi, LEDC PWM, and WASM BLE RGB controller.
- `feat(shell)`: `bt gatt` command to dump registered GATT services.
- `feat(bench)`: `akiraclaw_bench` hardware benchmark suite.
- `feat(security)`: `CONFIG_AKIRA_RELEASE_BUILD` fails configuration on development-only security settings: HTTP no-auth, empty or example upload token, the direct upload endpoint, unsigned apps, and unsigned or dev-key-signed MCUboot images. Development builds print the same findings as one warning.
- `feat(release)`: `west akira` command group (`keygen`, `sbom`, `sign`, `pack`, `release`) for product firmware — generate product signing keys (kept out of git via a written `.gitignore`), emit a CycloneDX 1.4 SBOM from the west manifest, sign the MCUboot image, forward app packaging to `akira-cli`, and assemble a `dist/` release (signed image + ELF + SBOM + `SHA256SUMS`). Registered through `west-commands` on the akira-os project so downstream products inherit it. A tag-triggered `.github/workflows/release.yml` builds, signs with the `MCUBOOT_KEY_PEM` secret, and uploads the artifacts to the GitHub release.

### Fixed / Security
- `fix(ble)`: Require an encrypted link for privileged management writes; make WASM app GATT services discoverable.
- `fix(security)`: Consistent `-EPERM` for capability denial; map `ota.trigger` capability string to its mask.
- `fix(wasm)`: Centralized, validated WASM pointer+length translation across HID/RF native APIs; decrement per-app memory quota on `mem_free`.
- `fix(ws)`: Connect `wss://` over TLS instead of silent cleartext.
- `fix(fs)`: Fail loud instead of diverting persistent writes to RAM.
- `fix(akiraconsole_prod)`: Fit `dram0.bss` and right-size the BT system heap to restore BLE.
- `fix(boards)`: Restore builds for rpi_pico(2), nucleo_h743zi, xiao_esp32c6.
- `fix(version)`: Firmware reported 1.6.2 instead of `VERSION`. CMake compile definitions overrode `AKIRA_VERSION_*`, and `akira.h` probed `<app_version.h>` rather than Zephyr's `<zephyr/app_version.h>`. OTA version reporting and anti-rollback now use the real version.
- `fix(app_manager)`: Stop filling the 16-bit `permissions` field from a 4-string parser whose bits did not match `AKIRA_CAP_*` (enforcement already used the 64-bit runtime mask). `akira caps` now shows the granted 64-bit mask instead of `heap_kb`; lifecycle APIs no longer truncate the mask to 32 bits.
- `fix(ota)`!: **Automatic rollback of a firmware update that fails to boot.** Before, it could not happen: `build.sh` built MCUboot overwrite-only on ESP32 (the Espressif SoC default), which destroys the previous image, and nothing confirmed a new image, so on swap-mode boards (STM32) every OTA reverted on its next reset. `build.sh` now builds MCUboot swap-using-move for ESP32/S2/S3 (`boards/mcuboot-swap-move.conf`), matching the app build; C3/C6/H2 stay overwrite-only (no bootloader IRAM headroom, swap not validated). `akira_boot_guard` is rewritten and on by default with `MCUBOOT_IMG_MANAGER`. It confirms a trial image `AKIRA_BOOT_GUARD_CONFIRM_DELAY_S` (30 s) after `AKIRA_HOOK_BOOT_READY`. It reboots the image, so MCUboot restores the previous firmware, on a fatal error, at the `AKIRA_BOOT_GUARD_TRIAL_TIMEOUT_S` (300 s) deadline, or on `akira_boot_guard_reject()`. On the next boot it reports the rollback (`akira_boot_guard_rolled_back()`, `AKIRA_HOOK_OTA_ERROR`, `ota status`). `ota_request_rollback()` now actually rolls back, to the previous firmware kept in slot1, instead of only setting a status string. Removed: the NVS boot counter, `CONFIG_AKIRA_OTA_MAX_BOOT_FAILURES` and `akira_boot_guard_count()`. They needed a `boot_counter` partition that no board defines, so the old guard never built.
- `fix(stack)`: App listings put `app_info_t` arrays (608 B per entry) on thread stacks smaller than the array: shell app list, `akira apps`/`akira caps`/memory report (4096 B shell stack), HID get-apps (4096 B system workqueue), the HTTP apps route (6144 B), and the WASM `app_list` API (8192 B app stack). They now use the new `app_manager_list_alloc()`, which heap-allocates (PSRAM first) and also returns SD-XIP apps that the fixed 8-entry HID list dropped.

### Changed
- `fix(version)`: Derive `AKIRA_VERSION_*` from the `VERSION` file.
- `ci`: Codecov no-regression coverage gate.
- `refactor(ui)`: Remove the unused stub UI framework.
- `ci`: Pin the Zephyr CI container to `ci:v0.28.6` by digest.
- `build`: Remove no-op `KCONFIG_WARN_UNDEF`/`KCONFIG_WERROR` CMake cache overrides (kconfiglib reads these from the environment).
- `deprecate(lib)`: `parse_capabilities_mask()` in `simple_json.h`; use `manifest_parse_json()`.
- `docs`: Product branches (KeyaPlatform, AkiraEar, Latch) are frozen; see CONTRIBUTING.
- `build`!: **AkiraOS is a real Zephyr module.** Its options moved to `zephyr/Kconfig` behind `CONFIG_AKIRA_OS` (default `n`) and its sources to `zephyr/CMakeLists.txt` (`akira_wamr` and `akira_os_core` libraries plus an `akira_os` interface target). **Applications must set `CONFIG_AKIRA_OS=y`.** The repository root is now a thin reference app; the boot sequence is `akira_start()` (`src/akira_os.c`), so product firmware in its own repository can link `akira_os` and reuse it. MCUboot and other images that only use the module's boards are unaffected.
- `build`!: WAMR and TFLite Micro are west projects under `modules/lib/` instead of git submodules; run `west update`. `-DMODULE_EXT_ROOT` is no longer needed. Existing `AkiraOS/modules/*` checkouts are ignored, not deleted.
- `samples`: `out_of_tree_product` shows product firmware consuming AkiraOS as a module; CI builds it on native_sim.
- `feat(extensibility)`!: **capability registry.** Capabilities are now registry entries (`AKIRA_CAPABILITY_DEFINE`, `include/akira_capability.h`); core bit numbers 0–47 are frozen, products define their own in bits 48–63 with build-time collision detection. `akira_capability_str_to_mask()` reproduces every previous string exactly; `"*"` and the sanitize path use the registry's known mask, so unassigned bits are never granted. The runtime refuses to start on a colliding registry. Also fixes the external-manifest merge that bypassed the capability clamp, and three AES native signatures (`(**i*~*)i` → 5 params).
- `feat(extensibility)`: **native API registry.** WASM natives are registered with `AKIRA_NATIVE_API_DEFINE()` (`include/akira_native_registry.h`) next to the functions they export, instead of one central `#ifdef` array; products add natives the same way. A native defined twice stops the runtime. The tables moved to ROM and are copied to PSRAM at start-up, saving ~2 KB DRAM on akiraconsole_prod.
- `feat(extensibility)`: **system hooks.** `AKIRA_HOOK_DEFINE()` (`include/akira_hooks.h`) delivers boot, app-lifecycle, connectivity and OTA events, so product firmware extends AkiraOS without editing the boot sequence. The legacy `akira_on_app_*` weak hooks still fire, through a compatibility shim.
- `security`!: On a hardened board (`CONFIG_AKIRA_REQUIRE_SIGNED_APPS=y` with a narrowed `CONFIG_AKIRA_UNSIGNED_APP_CAP_MASK`), a signed app must **embed** its manifest in the WASM binary (AkiraSDK `embed_manifest.py`) for its capabilities to be granted; a sidecar/package-only manifest is now clamped like any unattested request, because it is re-read from the filesystem at start and is not covered by the app signature. Default builds (`REQUIRE_SIGNED_APPS=n`, all-ones unsigned mask) are unaffected. This also removes the pre-1.6.5 unclamped `|=` merge of sidecar capabilities, an escalation path.
- `feat(security)`: **versioned WASM ABI.** Firmware exports `AKIRA_WASM_ABI_VERSION` (`include/akira_abi.h`); an app's `"abi"` manifest key is checked at install and load — a major mismatch is refused, a newer minor warns, a missing stamp is treated as legacy 1.x. `min_akiraos_version` is now enforced. akpkg manifests ≥ 4096 B are rejected. CI check `scripts/check_wasm_abi.py` fails on new drift between the firmware natives and the SDK header.
- `build`: split the per-board snippet into `akira-board` (hardware/SoC/storage) and `akira-reference` (the reference firmware's service selection). A product now pairs `akira-board` with a service profile (`akira-profile-*`), so any profile builds on any board; the reference firmware applies both and its `.config` is byte-identical on all 17 boards. Residual: on upstream devkits the WiFi driver is Zephyr-defined and not auto-enabled by a profile.
- `build`: per-board AkiraOS setup (flash layout, storage nodes, tuning) and the display panel moved from the reference app's `boards/` into module snippets (`snippets/akira-board`, `snippets/akira-display-*`); products apply them with `AKIRA_SNIPPETS`/`west build -S`. `.config` and devicetree are byte-for-byte identical for all 17 boards (`scripts/check_board_snippet.sh`).
- `build`: `build.sh` no longer copies MCUboot linker patches into the Zephyr tree for ESP32-C6/H2; it passes `CONFIG_CUSTOM_LINKER_SCRIPT` instead, so the Zephyr checkout is never modified.
- `docs`: maker-facing signing/release guide, a support & compatibility policy (release channels, LTS, and the firmware ↔ WASM-ABI ↔ SDK matrix), and per-SoC porting notes (ESP32-S3/C3/C6/H2, Nordic nRF54L15, STM32 H7/L4/U5, RP2040/RP2350).

---

## [1.5.8] — 2026-05-29

### Added
- `feat(connectivity SPI)`: USB CDC serial interface (`usb_cdc_serial`) and BLE companion service for host pairing.
- `feat(power)`: BQ28Z610 fuel gauge driver with DTS binding (`ti,bq28z610`).
- `feat(runtime)`: Pre-allocate WASM thread stacks via `SYS_INIT` to prevent heap fragmentation at runtime.
- `feat(boot animation)`: Boot animation configurable via `CONFIG_AKIRA_BOOT_ANIMATION`, fps and colors tunable via Kconfig.
- `feat(boards)`: nucleo_l476rg board support.
- `feat(security)`: Watchdog hardening, HTTP upload auth, signing enforcement; drop legacy local web UI.
- `feat(connectivity)`: USB web serial interface and BLE companion service.

### Fixed
- `fix(security)`: Missing `NETWORK`/`SETTINGS` entries in capability name lookup table.
- `fix(storage)`: SD hot-plug — deinit before reinit, retry on init failure, case-insensitive `.wasm`/`.aot` scan.
- `fix(wdt)`: Replace deprecated `wdt_enable()` with `wdt_setup()` (Zephyr 4.3 API).
- `fix(connectivity)`: Miscellaneous BT transfer, HTTP, OTA, and settings fixes.
- `fix(build)`: All-board build — resolve Kconfig orphans, web server link errors, and display guards.
- `fix(build)`: Boot animation source guard; drop `lvgl_input_driver` from OS shell build.
- `fix(ci)`: `dorny/test-reporter` git exit 128 in test job.
- `refactor(config)`: Use native Zephyr driver selectors for display panel (remove manual `#define` guards).
- `style(akpkg)`: Reformat `akpkg.c` to project K&R coding style.

### Changed
- Version bumped from 1.5.6 → 1.5.8 across all files.

---

## [1.5.6] — "C1PH3R" — 2026-05-14

### Added
- Track B: WASM native APIs for Filesystem (`akira_fs_api`), Crypto (`akira_crypto_api`), and RTC (`akira_rtc_api`) with capability-gated sandbox.
- Track A: OTA delta update engine (`akira_delta`), boot guard with automatic rollback (`akira_boot_guard`), and OTA WASM native API (`akira_ota_api`).
- Track D: `akira` shell command tree (`akira_runtime_cmds`), structured telemetry sink (`akira_telemetry`), and panic handler with NVS crash storage (`akira_panic`).
- New capability bits 26–31: `AKIRA_CAP_FS_READ/WRITE`, `AKIRA_CAP_CRYPTO`, `AKIRA_CAP_RTC_READ/WRITE`, `AKIRA_CAP_OTA_TRIGGER`.
- `docs/ota_design.md` — OTA architecture design document.

### Fixed
- SD card hot-plug: if the card was absent at boot, inserting it later now works. `sd_manager_mount()` re-probes via `akira_sd_card_init()` instead of returning `-ENODEV` immediately, and `fs_manager_reinit_sd()` updates the availability flag so all SD paths become accessible without reboot.
- `ota_request_rollback()` implemented (was declared but never defined).
- `ota_manager.c` compile guard aligned with `CONFIG_AKIRA_OTA` (fixes undeclared `CONFIG_AKIRA_OTA_THREAD_STACK_SIZE` on boards with OTA disabled).
- Pre-existing warnings: nested `/*` in Doxygen comments, `printf` format mismatch (`off_t` vs `%x`), unused static functions.
- All WASM allocations migrated to `akira_malloc_buffer()` / `akira_free_buffer()`.

### Changed
- Version bumped from 1.5.4 → 1.5.6 across all files.
- Log banner updated to `AkiraOS v1.5.6 `.

---

## [1.5.4] — 2026-04-29

### Added
- API stability annotations (`@stability`, `@since`) on every public header.
- `docs/api-stability-policy.md` — formal 2-release deprecation policy.
- Cooperative scheduling model and watchdog contract documented in
  `docs/architecture/scheduling-and-watchdog.md`.
- Full ztest suite (54 tests) covering security, OTA, app lifecycle,
  manifest parser; GitHub Actions CI with gcov/Codecov coverage badge.

### Changed
- Version bumped from 1.5.1 → 1.5.4 across all files.
- Log banner updated to `AkiraOS v1.5.4 - Hardened Runtime`.

---

## [1.4.9] — "Gl1tch" — 2026-03-21

> **Release branch:** `v1.4.x` | **PR:** [#52](https://github.com/ArturR0k3r/AkiraOS/pull/52)
> 125 commits · 350 files · ~40,600 insertions

This release completes the v1.4.x generation of AkiraOS. It replaces the legacy OCRE runtime with WAMR, introduces the Capability Guard security model, brings up the AkiraConsole custom board, adds a full suite of WASM peripheral APIs, and ships the AkiraSDK as an independently versioned git submodule.

**Closes:** [#55](https://github.com/ArturR0k3r/AkiraOS/issues/55) · [#54](https://github.com/ArturR0k3r/AkiraOS/issues/54) · [#44](https://github.com/ArturR0k3r/AkiraOS/issues/44) · [#19](https://github.com/ArturR0k3r/AkiraOS/issues/19) · [#13](https://github.com/ArturR0k3r/AkiraOS/issues/13)

---

### Runtime Engine

#### Added
- **WAMR integration** — complete migration from OCRE to WebAssembly Micro Runtime (WAMR) with Zephyr 4.3.0 compatibility and `native_sim` support
- **Thread-per-app model** — each WASM app runs in its own Zephyr thread; lifecycle managed via `SYS_INIT` hooks
- **AOT support** enabled by default — native-level execution speed with ~60 KB RAM savings over interpreter mode
- **Chunked WASM loading** with automatic PSRAM fallback on ESP32-S3; prevents boot-time OOM
- **Instance pool** — pre-allocated memory slots avoid repeated `malloc`/`free` cycles during app churn
- **Module cache** — SHA-256-keyed hash map skips redundant reloads of identical WASM binaries
- **Execution watchdog** — per-slot configurable timeout terminates runaway apps automatically
- **Per-app memory quotas** enforced inline at `NativeSymbol` dispatch layer
- **Inline capability checks** — `AKIRA_CAP_*` bitmask validated before every native export call
- `wamr_config.h` — central WAMR compile-time configuration header
- `runtime_cache.h` — module cache and hash-map slot-lookup types

#### Changed
- WASM compile target switched from **WASI** to bare **`wasm32`** with self-contained `printf`; eliminates WASI libc dependency in all SDK apps
- `akira_runtime.c` fully rewritten — removed all legacy OCRE glue, magic numbers, and duplicate loader paths
- Settings manager reworked to flash-backed persistence; WiFi credentials now pulled from settings at runtime

#### Fixed
- WASM out-of-bounds memory trap on ESP32-S3 during certain API calls
- App lifecycle freezes when stopping and restarting the same WASM module
- Boot crash when SD card is absent but storage API is enabled
- App install stack overflow on ESP32-S3 (increased dedicated install thread stack)
- DRAM overflow on boards without PSRAM (reduced static allocations; fallback paths added)
- BT mode conflict when BLE and BT HID are active simultaneously
- Poll stack overflow under concurrent WiFi and web server traffic

---

### Security

#### Added
- **Capability Guard model** — every native API export guarded by `akira_security_check_exec()` with `AKIRA_CAP_*` bitmask; manifests declare required capabilities
- **SHA-256 binary integrity** — mbedTLS-backed hash verified before any WASM binary is loaded into the runtime
- **Ed25519 / RSA signature verification** — `src/runtime/security/app_signing_v2.c` validates app publisher signatures via mbedTLS
- **Runtime sandbox** — `sandbox.c` / `sandbox.h` enforce syscall filtering and per-app call rate limiting
- **Security audit log** — capability violations and sandbox events written to ring buffer for post-mortem inspection
- `security.c` / `security.h` — unified capability authority (replaces the two divergent `security.h` / `capability.h` files)

#### Fixed
- Multiple redundant `#define` declarations for capability constants across headers
- `AKIRA_CAP_*` symbols were declared in both `security.h` and `capability.h` — now single source of truth

---

### Connectivity

#### Added
- **Arduino-style BLE App API** — replaces raw `bt_shell`; full 12-function surface: `ble_init`, `ble_deinit`, `ble_set_local_name`, `ble_service_create`, `ble_char_create`, `ble_service_add_char`, `ble_add_service`, `ble_set_advertised_service`, `ble_advertise`, `ble_stop_advertise`, `ble_char_write`, `ble_event_pop` and more
- **BLE GATT service pool** — multi-service, multi-characteristic layout with CCC/notify plumbing; event queue (`K_MSGQ`) for async connect/disconnect/write events
- **BT HID WASM API** — hotkey scripting and custom key bindings from WASM apps (`bt_hid_send_key`, `bt_hid_send_media`)
- **USB HID mouse support** added alongside existing keyboard (`feat(usb_hid): added mouse to usb_hid`)
- **TCP/UDP WASM network API** — zero-copy ring-buffer shared-memory channel; `net_tcp_connect`, `net_tcp_send`, `net_udp_send`, `net_recv`
- **Shared HTTP upload buffer pool** — prevents fragmentation during concurrent OTA + web-server traffic
- Matter, Thread, and AkiraMesh connectivity layer scaffolding

#### Changed
- WiFi scan (`wifi_scan` shell command) now blocks for 5 s and prints results inline instead of to a background log
- HTTP web server refactored to callback-based transport layer; stack size increased
- USB HID manager ported to the new Zephyr USB stack

#### Fixed
- BLE HID iOS compatibility — corrected HID descriptor to separate keyboard from composite device mode
- BT HID report descriptor misalignment causing dropped keys
- Web server crash on first HTTP request after boot
- OTA partition layout — NVS/LFS partitions relocated above OTA region; stale boot counter healed on upgrade
- Net buffer exhaustion under concurrent WiFi + web server: `NET_PKT RX/TX` raised 8→12, `BUF_TX` raised 16→20

---

### Hardware & Board Support

#### Added
- **AkiraConsole** custom out-of-tree ESP32-S3 board — full HWMv2 definition: `boards/akiraconsole/` with `.dts`, pinctrl `.dtsi`, `.yaml`, `_defconfig`, `board.cmake`, `board.yml`, and per-board `.conf` / `.overlay`
- **ESP32-S3 Super Mini** out-of-tree board — HWMv2-compatible definition in `boards/esp32s3_super_mini/`
- **`akira,ili9341`** out-of-tree Zephyr display driver for AkiraConsole (`feat(display): add akira,ili9341 out-of-tree driver`)
- **LR1121 transceiver** RF framework driver with `SYS_INIT` auto-init hooks (`src/drivers/rf/`)
- **SD card** support — `sd_card.c` / `sd_card.h`; direct app install from SD; WASM storage API on top
- `boards/lr1121_template.overlay` — reference overlay for LR1121 radio integration
- `boards/nrf54l15dk_nrf54l15_cpuapp` board files added to overlay set

#### Changed
- ST7789V display integrated with Zephyr native driver and dynamic capability registration
- All board `.conf` / `.overlay` files refactored for strict modularity; board-specific settings removed from root `prj.conf`
- LSM6DS3 IMU driver moved to out-of-tree with generic sensor WASM API facade

#### Fixed
- STM32 B-U585I-IOT02A boot sequence; app image now signed for MCUBoot before flash
- ESP32-S3 Super Mini board definition updated for Zephyr 4.3 HWMv2 requirements
- ESP32 DevKitC board configs cleaned up; removed duplicated symbols

---

### WASM APIs

New native functions registered into the WASM sandbox (`akira_export_api.c`):

| Domain | New Exports |
|---|---|
| GPIO / Input | `gpio_set`, `gpio_get`, `button_event_pop` |
| Timer | `timer_create`, `timer_start`, `timer_stop`, `timer_delete` |
| UART / I2C / PWM | `uart_write`, `i2c_read`, `i2c_write`, `pwm_set` |
| Display | `display_fill`, `display_draw_rect`, `display_draw_line`, `display_draw_circle`, `display_blit` |
| Sensor | `sensor_read`, `sensor_start`, `sensor_stop` |
| BLE | Full BLE App API (12 functions, see Connectivity) |
| BT HID | `bt_hid_send_key`, `bt_hid_send_media`, `bt_hid_send_consumer` |
| Network (TCP/UDP) | `net_tcp_connect`, `net_tcp_send`, `net_udp_bind`, `net_udp_send`, `net_recv`, `net_close` |
| Storage / FS | `fs_open`, `fs_read`, `fs_write`, `fs_seek`, `fs_close`, `fs_list`, `fs_stat`, `fs_remove` |
| Power | `power_sleep`, `power_deep_sleep`, `power_get_battery_level`, `power_get_charge_state` |
| RF | `rf_send`, `rf_recv`, `rf_set_channel`, `rf_get_rssi` |
| App Lifecycle | `app_start`, `app_stop`, `app_get_state` |
| IPC | `ipc_send`, `ipc_recv` (inter-app messaging ring buffer) |

---

### AkiraSDK & WASM Apps

#### Added
- **`AkiraSDK/` git submodule** — replaces `wasm_sample/`; SDK is now independently versioned at its own repository
- SDK ships 17 pre-built cross-compiled WASM apps: `hello_world`, `gpio`, `ble_led`, `compass`, `cube3d`, `display_test`, `imu_3d`, `imu_timer_test`, `inclinometer`, `logic_analyzer`, `macro_pad`, `net_echo`, `net_server`, `retro`, `storage_test`, `supervisor`, `tetris`
- **Logic Analyzer 2.0** — fully rewritten 4-channel logic analyzer app
- **Tetris** — complete playable game for AkiraConsole display
- **IMU visualization suite** — 3D IMU viewer, inclinometer, compass, and timer-based IMU test apps
- SDK `Makefile` supports bare `wasm32` cross-compilation and AOT pre-compilation targets

#### Changed
- All SDK apps ported from WASI to bare `wasm32` target with self-contained `printf`
- `AkiraSDK/docs/` content deduplicated — single source of truth; OS-level docs live only in the main repo

---

### Shell & Settings

#### Added
- `shell_rf.c` — RF shell commands: channel set, TX/RX test, RSSI readout
- `shell_display_cmds.c` — display shell commands: fill, clear, test patterns
- `shell_web.c` — web server shell commands: start, stop, status
- `shell_sys.c` — system info shell commands (migrated from legacy `akira/shell.c`)

#### Fixed
- Cold reboot issue — reboot now properly resets all subsystem state before `sys_reboot()`
- AOT mode not persisting across reboots — AOT config now saved to NVS via settings manager
- App and main stack limits raised for Retro game workload (`fix(shell,config)`)
- `build.sh` ANSI color codes not rendering in `--help` on some terminals

---

### Build System & CI/CD

#### Added
- `Dockerfile.ci` — hermetic Docker image for reproducible CI builds; Espressif toolchain pre-installed
- `.github/workflows/ci.yml` — parallel board build matrix: ESP32-S3, ESP32-C3, STM32 B-U585I, nRF54L15, Native Sim
- `.github/ISSUE_TEMPLATE/bug_report.md` and `feature_request.md`
- `.github/PULL_REQUEST_TEMPLATE.md`
- `build.sh` board auto-discovery — detects available `boards/` overlays without manual specification

#### Changed
- Zephyr SDK version bumped to **0.17.4** in CI
- Old `.github/workflows/build.yml` replaced by the new `ci.yml` matrix pipeline
- `build.sh` fully rewritten — cleaner argument parsing, better error messages, board list auto-generation

#### Fixed
- Docker `exec` environment variable passthrough for Espressif toolchain paths
- STM32 boot: app image signing step added before west flash

---

### Documentation

#### Added
- Full **GitHub Pages site** at [docs.akiraos.dev](https://docs.akiraos.dev) — Jekyll + Just the Docs theme
- Structured docs tree:
  - `docs/api-reference/` — native API reference, manifest format, error codes
  - `docs/architecture/` — AOT compilation guide, advanced connectivity, runtime internals
  - `docs/getting-started/` — quickstart, hardware setup, first WASM app
  - `docs/development/` — build options, SDK usage, contributing guide
  - `docs/hardware/` — supported boards, AkiraConsole schematics
  - `docs/platform/` — Zephyr integration, WAMR internals
  - `docs/resources/` — FAQ, glossary, performance benchmarks
- `docs/_config.yml` — Jekyll site configuration (Just the Docs, dark theme, mermaid diagrams)
- `assets/AKIRAOS-LOGO.png` and `assets/logo.png` — branding assets
- `CODE_OF_CONDUCT.md`

#### Changed
- `README.md` significantly expanded with architecture overview, board matrix, and quick-start instructions
- `QUICKSTART.md` rewritten — accurate West manifest commands, board-specific build instructions, SDK usage
- `CONTRIBUTING.md` updated with v1.4.x branch conventions and PR template guidance

#### Removed
- Flat legacy doc files: `docs/ARCHITECTURE.md`, `docs/CONNECTIVITY.md`, `docs/HARDWARE.md`, `docs/APP_DEVELOPMENT_GUIDE.md`, `docs/APP_MANAGER.md`, `docs/PENDING_FEATURES.md`, `docs/SUPPORTED_BOARDS.md`, `docs/WASM_QUICK_REFERENCE.md` — all superseded by the new structured docs site
- `docs/AkiraConsole/` and `docs/AkiraMicro/` binary assets (STL + PDF) removed from repo; hosting moved externally

---

### Performance

| Metric | Before (OCRE) | After (WAMR + AOT) |
|---|---|---|
| RAM overhead per app slot | ~120 KB | ~60 KB |
| WASM execution speed | Interpreted | Native (AOT) |
| Boot time to first app (ESP32-S3) | ~4.2 s | ~1.8 s |
| Net buffer headroom under peak WiFi | Marginal | Stable (raised pools) |

---

### CI Matrix

| Board | Build | Tested on HW |
|---|---|---|
| `native_sim` | Passing | Yes (exe) |
| `esp32s3_devkitm_esp32s3_procpu` | Passing | Yes |
| `akiraconsole_esp32s3_procpu` | Passing | Yes |
| `esp32s3_super_mini_esp32s3_procpu` | Passing | Yes |
| `b_u585i_iot02a` | Passing | Yes |
| `esp32_devkitc_procpu` | Passing | Yes |
| `esp32c3_devkitm` | Passing | Yes |

---

## [1.3.8] — "AkiraOS-v1.3.8-GL1TCH"

Legacy OCRE-based release. Superseded by v1.4.9.

---

## [1.2.3] — 2024

Initial public release with OCRE runtime, basic BLE, WiFi OTA, and LittleFS storage.
See branch `v1.2.3` for history.
