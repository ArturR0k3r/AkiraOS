# AkiraClaw — Edge AI Developer Guide

AkiraClaw is the on-device ML inference subsystem for AkiraOS.  It exposes a
three-function WASM API (`aiinfer_load` / `aiinfer_run` / `aiinfer_unload`)
backed by [TFLite Micro](https://www.tensorflow.org/lite/microcontrollers),
gated behind the `ai.infer` capability in the app manifest.

---

## Quick-start

### 1 — Declare the capability in `manifest.json`

```json
{
  "name": "my_ai_app",
  "version": "1.0.0",
  "capabilities": ["display.write", "sensor.read", "ai.infer", "memory"],
  "memory_quota": 131072
}
```

### 2 — Call the API in `main.c`

```c
#include "akira_api.h"

extern const unsigned char g_model[];    /* linked-in model bytes  */
extern const unsigned int  g_model_len;

int main(void)
{
    int handle = aiinfer_load(g_model, (int)g_model_len);
    if (handle < 0) { /* handle errors */ return -1; }

    int8_t input[INPUT_SIZE];
    int8_t output[OUTPUT_SIZE];
    /* ... fill input ... */

    int ret = aiinfer_run(handle, input, INPUT_SIZE, output, OUTPUT_SIZE);

    aiinfer_unload(handle);
    return ret;
}
```

### 3 — Pack the model into the `.akpkg`

```sh
# Compile
clang -O2 -nostdlib -Wl,--no-entry -Wl,--export=main -Wl,--allow-undefined \
    main.c -o my_ai_app.wasm

# Bundle model with app
akira-cli pack my_ai_app.wasm manifest.json --model model.tflite

# Sign
akira-cli sign my_ai_app.akpkg --key privkey.pem
```

---

## API Reference

### `int aiinfer_load(const void *model, int model_size)`

Loads a TFLite flatbuffer into an inference slot.  The model is copied
internally; the caller may free the source buffer after this call returns.

**Returns:** Non-negative slot handle (0–3) on success.

| Error code | Value | Meaning |
|---|---|---|
| `AIINFER_ERR_NOMEM`   | -1 | Arena or model copy OOM |
| `AIINFER_ERR_INVALID` | -2 | Bad pointer / schema mismatch |
| `AIINFER_ERR_NOSLOT`  | -4 | All `AKIRA_AIINFER_MAX_HANDLES` slots occupied |

**Kconfig:** slot limit is `CONFIG_AKIRA_AIINFER_MAX_HANDLES` (default 4).

---

### `int aiinfer_run(int handle, const void *input, int input_size, void *output, int output_size)`

Runs one inference pass.  The function:
1. Copies `input_size` bytes into the model's input tensor.
2. Calls `MicroInterpreter::Invoke()`.
3. Copies the output tensor bytes into `output`.

`input_size` must exactly equal the model's input tensor byte count.
`output_size` must be ≥ the model's output tensor byte count.

**Returns:** 0 on success.

| Error code | Value | Meaning |
|---|---|---|
| `AIINFER_ERR_INVALID` | -2 | Bad handle or Invoke() failed |
| `AIINFER_ERR_SHAPE`   | -3 | input/output size mismatch |

---

### `void aiinfer_unload(int handle)`

Releases the slot, destroys the interpreter, and frees the model copy.
The handle must not be used after this call.

---

## Model Packaging

The `.akpkg` format is a gzip-compressed tar archive.  When you pass
`--model model.tflite` to `akira-cli pack`, the model is added as the
`model.tflite` tar entry and its bytes are included in the signing digest:

```
SHA-256(manifest.json || app.wasm || model.tflite)
```

On installation, the firmware copies `model.tflite` to
`/lfs/apps/<app_name>/model.tflite` on the device filesystem, from where
the app loads it at runtime via `storage_open`.

### Supported model formats

| Format | Notes |
|---|---|
| INT8-quantized TFLite flatbuffer | Recommended — smallest RAM, fastest on all targets |
| FLOAT32 TFLite flatbuffer | Works but uses more arena RAM and is slower |

Models must use operators registered in `akira_aiinfer_api.cpp`.  The
default build registers: `DEPTHWISE_CONV_2D`, `CONV_2D`, `FULLY_CONNECTED`,
`SOFTMAX`, `RESHAPE`, `AVERAGE_POOL_2D`, `MAX_POOL_2D`, `QUANTIZE`,
`DEQUANTIZE`.  Add more via the resolver in the `.cpp` file if needed.

---

## Target RAM & Latency Guide

Tensor arena size is set by `CONFIG_AKIRA_AIINFER_ARENA_KB` (default 128 KiB).
Reduce it on RAM-constrained targets; increase it for larger models.

| Target | Accelerator | DS-CNN KWS latency (est.) | Min arena |
|---|---|---|---|
| ESP32-S3 + ESP-NN | ESP-NN SIMD (`CONFIG_AKIRA_TFLITE_ESPNN=y`) | ~40 ms | 80 KiB |
| STM32H7 + CMSIS-NN | CMSIS-NN MVE (`CONFIG_AKIRA_TFLITE_CMSIS_NN=y`) | ~25 ms | 80 KiB |
| nRF54L15 + CMSIS-NN | CMSIS-NN DSP | ~60 ms | 64 KiB |
| Generic Cortex-M4 | Reference kernels only | ~150 ms | 64 KiB |

Acceleration is auto-selected by Kconfig defaults based on `SOC_SERIES` /
`CPU_CORTEX_M` symbols — no manual configuration needed on known boards.

---

## Arena Sizing

The tensor arena must fit:
- All input/output tensors simultaneously
- All intermediate activation tensors

A safe rule of thumb: **arena ≥ 2× the largest model layer activation size**.
For KWS/audio models (DS-CNN family), 80–128 KiB covers most cases.
For image-classification models (MobileNetV1 96×96 INT8), plan for 256–512 KiB.

Set arena size in `prj.conf`:

```kconfig
CONFIG_AKIRA_AIINFER_ARENA_KB=128
```

---

## Keyword Spotting Reference App

`AkiraSDK/wasm_apps/console_apps/kws_demo/` is the canonical AkiraClaw demo.

It detects the "hey akira" wake word and calls `app_switch("shell")`:

```
Pipeline:
  mic → 16 kHz ADC capture (1 s window)
      → log-mel spectrogram [49 × 40] INT8
      → DS-CNN model (INT8, ~20 KiB)
      → 2-class softmax [hey_akira, background]
      → confirm N consecutive frames above threshold
      → app_switch("shell")
```

Build and deploy:

```sh
cd AkiraSDK/wasm_apps/console_apps/kws_demo

# Provide a DS-CNN KWS model quantized to INT8 (Google Speech Commands dataset)
make pack MODEL=/path/to/ds_cnn_kws_int8.tflite
make sign KEY=/path/to/privkey.pem

akira-cli install kws_demo.akpkg --device 192.168.1.42:8080 --token <token>
```

The model is not checked in to this repo.  Train one with the
[TFLite Micro micro_speech example](https://github.com/tensorflow/tflite-micro/tree/main/tensorflow/lite/micro/examples/micro_speech)
or convert any Google Speech Commands DS-CNN checkpoint using
`tflite_convert` with `--inference_type=INT8`.

---

## Threat Model & Security Notes

See `docs/ai/security-model.md` (TODO) for the full threat model.  Key points:

- **Model integrity**: the model is covered by the Ed25519 package signature.
  A tampered model causes signature verification failure at install time.
- **Input isolation**: WASM memory bounds are enforced by WAMR.  A malicious
  app cannot pass a pointer outside its sandbox to `aiinfer_run`.
- **Capability gating**: `ai.infer` must be declared in the manifest.  Apps
  without this capability receive `-EPERM` from all `aiinfer_*` calls, and the
  denial is written to the audit log.
- **Arena isolation**: each slot has its own static arena; slots cannot access
  each other's tensor data.
- **Side-channel leakage**: timing side-channels on inference output are the
  app developer's responsibility.  For medical/industrial use, consider adding
  random delay jitter around `aiinfer_run` calls.
