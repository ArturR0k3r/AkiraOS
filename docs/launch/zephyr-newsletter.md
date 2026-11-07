Try it in your browser: https://console.app.akiraos.dev/simulator

# AkiraOS — a WebAssembly app platform built as a Zephyr module

AkiraOS (Apache-2.0) is an embedded OS that packages as a **Zephyr module** and runs
sandboxed WebAssembly applications via WAMR. Apps install over the air as `.wasm` modules,
isolated by a capability manifest enforced on every native call.

Of note to the Zephyr community:
- Ships as a Zephyr module (`zephyr/module.yml`); products build from their own repos with
  AkiraOS, WAMR and TFLite Micro pulled in as west projects.
- Builds for ESP32 (S3/classic/C3/C6/H2), nRF54L15, STM32 (U5/H7/L4) and RP2040/RP2350,
  plus `native_sim` for host iteration.
- MCUboot A/B updates with SHA-256 integrity and boot-failure rollback; a SESIP Level 2
  evaluated configuration.
- WAMR classic interpreter for portability, optional AOT per architecture.

A browser simulator (link above) runs the real app binaries for a zero-install demo.

Repo: https://github.com/ArturR0k3r/AkiraOS · Docs: https://docs.akiraos.dev
