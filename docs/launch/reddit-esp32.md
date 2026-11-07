Try it in your browser (no install): https://console.app.akiraos.dev/simulator

# AkiraOS on ESP32-S3: install WASM apps over the air, no reflash

AkiraOS is an open-source (Apache-2.0) OS on Zephyr that runs sandboxed WebAssembly apps.
On ESP32-S3 you flash the OS once, then install/update apps as `.wasm` over WiFi, BLE or
USB — no esptool round-trip to change what the device does.

ESP32 specifics:
- Targets: ESP32-S3 (DevKitM + the AkiraConsole board), classic ESP32, and C3/C6/H2.
- Prebuilt firmware is attached to each GitHub release.
- Apps are isolated by a capability manifest; an app only gets the GPIO/I2C/BLE/etc. it
  declares.
- Optional AOT compilation to Xtensa native code for compute-heavy apps.

Honest status: pre-1.x; default builds unsigned (there's a `--release` signing flow and
flash-encryption / Secure Boot V2 are integrator steps). No published benchmarks yet.

The link above runs AkiraConsole apps in the browser — drop in your own `.wasm` to see the
API. Repo: https://github.com/ArturR0k3r/AkiraOS

Curious what people here think about the OTA-app model vs. ESP-IDF OTA of full firmware.
