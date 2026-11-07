Try it in your browser (no install): https://console.app.akiraos.dev/simulator

# Show HN: AkiraOS — sandboxed WebAssembly apps for microcontrollers, deployed over the air

Hi HN. AkiraOS is an open-source (Apache-2.0) embedded OS built on Zephyr + the
WebAssembly Micro Runtime. The idea: stop reflashing firmware to change what a device
does. Apps are `.wasm` modules you install over WiFi/BLE/USB; the OS and bootloader stay
put. Each app runs in a WASM sandbox and can only call the native APIs its manifest
declares — a 64-bit capability mask the runtime checks on every call.

The link above is a browser simulator running the same `.wasm` an AkiraConsole device
runs — load a catalogue app or drop in your own. There's also an app catalogue and an SDK
(C fully supported; Rust and MicroPython experimental).

Why I built it: firmware updates are all-or-nothing and a bad build can brick a device.
Treating apps as sandboxed, OTA-deployable modules — the "Expo for microcontrollers"
model — makes iteration and third-party apps safe. The same `.wasm` runs across ESP32-S3,
nRF54L15 and RP2350; ship an optional AOT build per architecture for native-code speed.

Honest status: it's pre-1.x. Default builds are unsigned (there's a `--release` signing
flow and a SESIP Level 2 evaluated config). I haven't published benchmarks yet — the
capability-check and interpreter/AOT numbers aren't measured, and I'd rather say so than
make them up.

- Repo: https://github.com/ArturR0k3r/AkiraOS
- Docs: https://docs.akiraos.dev
- Reference hardware (AkiraConsole): https://akiraos.dev/akiraconsole

Happy to answer questions about the capability model, WAMR on Zephyr, or how this compares
to MicroPython / Toit / Ocre.
