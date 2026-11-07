# AkiraOS Roadmap

Where AkiraOS is going. Grouped as **Now** (in progress), **Next** (planned, not
started) and **Later** (wanted, not scheduled). This is a direction, not a dated
commitment — dates and scope move. The authoritative record of what shipped is
[`CHANGELOG.md`](CHANGELOG.md).

Want to help? Many of these are tagged [`good first issue` / `help wanted`](https://github.com/ArturR0k3r/AkiraOS/labels/good%20first%20issue).

## Now

- **One-click browser simulator from the repo.** README and QUICKSTART lead with the
  [simulator](https://console.app.akiraos.dev/simulator); next is a demo GIF and
  per-app "Run in browser" deep links once the catalogue is populated.
- **Prebuilt firmware on every release.** CI builds `akiraconsole`, `akiraconsole_prod`,
  `esp32s3_devkitm`, `nrf54l15dk` and `rpi_pico2` and attaches them to the GitHub release.
- **Honest docs.** Performance figures consolidated and marked measured / not-yet-measured;
  one language-support list; all links checked.
- **App catalogue population.** The AkiraSDK release workflow packages every C/console/retro
  app into the catalogue the simulator and app store read.

## Next

- **Web flasher** (ESP Web Tools): connect a board over USB from the browser → flash →
  WiFi provisioning, hosted at `docs.akiraos.dev/flash` or inside the app site.
- **`akira` developer CLI**: `new` (project scaffolding for C / Rust / Python), `build`,
  `deploy [--device <ip> | --sim | --web]`, `devices` (mDNS), `logs`, `apps list|remove`,
  `publish`. (A Go `akira-cli` already does pack/sign/install/update; this extends it.)
- **App submission with authentication.** The catalogue submission endpoint currently
  accepts uploads without auth — add a token / moderation flow before wide launch.
- **Rust and MicroPython from experimental to supported**: ship a `micropython.wasm` with
  the `_akira` module, and a `akira new --lang rust` template.
- **Deep-link "Run in browser"** on every SDK sample and catalogue app.

## Later

- **More language bindings**: Zig, TinyGo, AssemblyScript.
- **Published benchmarks**: Capability Guard overhead and interpreter/AOT/native ratios
  from `bench/akiraclaw_bench`, with board, clock and method.
- **More board ports** and a community hardware gallery.
- **Richer simulator fidelity**: model more peripherals (sensors, RF capture) instead of
  returning `-ENOSYS`.
- **AkiraConsole on CrowdSupply.**

---

*Have something you want to see here? Open a
[discussion](https://github.com/ArturR0k3r/AkiraOS/discussions) or an issue.*
