Browser demo (no install): https://console.app.akiraos.dev/simulator

# AkiraOS: sandboxed WASM apps on Zephyr, installed over the air

I've been building AkiraOS, an Apache-2.0 embedded OS on top of Zephyr RTOS and WAMR.
Apps are WebAssembly modules installed over WiFi/BLE/USB instead of firmware reflashes.
Each app is sandboxed and gated by a capability manifest — it can only touch the
peripherals it declared, enforced on every native call.

What might interest this sub specifically:
- WAMR on Zephyr, classic interpreter for `.wasm` plus optional AOT (`.aot`) per arch.
- One app binary across Xtensa (ESP32-S3) and ARM (nRF54L15, STM32, RP2350).
- MCUboot A/B slots with SHA-256 integrity and boot-failure rollback.
- The closest relative is Ocre (also WAMR-on-Zephyr); the differences are the capability
  model, reference hardware, an app catalogue and a browser simulator.

Honest status: pre-1.x. Default builds are unsigned (signing is a `--release` flow). No
published benchmarks yet — I won't quote numbers I haven't measured.

Repo: https://github.com/ArturR0k3r/AkiraOS · Docs: https://docs.akiraos.dev

Feedback on the capability model and the OTA/rollback design is very welcome.
