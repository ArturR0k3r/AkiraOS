Browser demo (no hardware needed): https://console.app.akiraos.dev/simulator

# Tip: AkiraOS — "install apps, don't reflash firmware" for microcontrollers

Short version for the tip line:

AkiraOS is an open-source (Apache-2.0) OS built on Zephyr and WebAssembly. Instead of
reflashing firmware, you install sandboxed `.wasm` apps over WiFi/BLE/USB. Each app is
confined by a capability manifest — it only reaches the hardware it declared. The same app
binary runs on ESP32-S3, nRF54L15 and RP2350.

The hook for readers: there's a browser simulator (link above) that runs the real app
binaries — no install, no hardware — plus an app catalogue and reference hardware
(AkiraConsole, OSHWA-certified MD000003). It's the "Expo for microcontrollers" idea applied
to firmware.

Honest status: pre-1.x, default builds unsigned, no published benchmarks yet.

- Repo: https://github.com/ArturR0k3r/AkiraOS
- Hardware: https://akiraos.dev/akiraconsole
- Maker/contributor guide: https://github.com/ArturR0k3r/AkiraOS/blob/main/CONTRIBUTING.md
