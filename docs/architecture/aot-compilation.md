# AOT Compilation Architecture

**Ahead-of-Time compilation for high-performance WebAssembly execution.**

## Overview

AkiraOS includes AOT (Ahead-of-Time) compilation support through WAMR, enabling WASM applications to be compiled to native machine code before deployment. This provides dramatically improved execution performance compared to bytecode interpretation.

### Current State

**AOT Compilation Disabled by Default:** WAMR AOT support is currently disabled via `CONFIG_WAMR_AOT_SUPPORT=n` in `prj.conf`.
**Default Mode:** Applications run in interpreter mode (.wasm bytecode).
**To Enable:** Rebuild the project with `CONFIG_WAMR_AOT_SUPPORT=y` in your Kconfig. When enabled, AOT execution is used when `.aot` files are present.

**Key Point:** AOT is a **compilation option**, not a runtime mode. The system can load both .wasm (interpreted) and .aot (native) files.

**Note:** While compiling AOT application you lose cross-platform so carefull consideration of using this option is needed.

---

## Performance Comparison

### Interpreter vs AOT

| Execution Mode | Speed | Memory | Universality |
|----------------|-------|--------|--------------|
| **Interpreter (.wasm)** | 1x baseline | Lower overhead | Universal binary |
| **AOT (.aot)** | **10-50x faster** | Slightly higher | Architecture-specific |

### Real-World Impact

**Compute-Heavy Apps:**
- Image processing: ~40x faster
- Physics simulations: ~25x faster
- Cryptography: ~15x faster
- Game logic: ~10-15x faster

**Benefits:**
- Near-native CPU performance — direct machine code execution
- Lower power consumption — reduced per-instruction overhead
- Smooth rendering — 60 FPS display, responsive game loops
- Predictable performance — no interpreter overhead variability

---

## Architecture-Specific Binaries

### The Tradeoff

AOT binaries are **native machine code** compiled for a specific CPU architecture. This means:

```
app.wasm (universal)
  ↓
wamrc --target=xtensa    →  app_esp32s3.aot   (ESP32-S3 only)
wamrc --target=thumbv7   →  app_stm32.aot     (STM32/ARM Cortex-M only)
wamrc --target=x86-64    →  app_native.aot    (Native sim only)
```

**Implications:**
- Maximum performance on target hardware
- One binary per architecture required
- Larger storage when supporting multiple platforms
- Runtime auto-detects and loads the correct binary

---

## Deployment Strategies

AkiraOS supports three AOT deployment models:

### 1. Interpreter-Only (.wasm)

**Use Case:** Universal apps, prototyping, low-memory devices

```
app.wasm  →  Upload to device  →  WAMR interprets bytecode
```

**Pros:**
- Single binary works on all platforms
- Smaller file size (no native code)
- Faster compile times during development
- Lower storage requirements

**Cons:**
- Slower execution (1x baseline)
- Higher CPU usage for the same workload
- Higher power consumption

**Best For:**
- Simple UI apps
- Configuration scripts
- Low-frequency sensor polling
- Prototyping and development

---

### 2. AOT-Only (.aot)

**Use Case:** Performance-critical apps, single-platform deployment

```
app.c  →  clang → app.wasm  →  wamrc → app.aot  →  Upload to device
```

**Pros:**
- Maximum performance (10–50x faster)
- Lower power consumption
- Optimal for compute-heavy workloads

**Cons:**
- Architecture-specific binary
- Larger file size (~2–3x bigger than .wasm)
- Must recompile for each target platform

**Best For:**
- Gaming engines
- Display rendering (60 FPS graphics)
- Real-time audio/video processing
- Machine learning inference
- Production apps on known hardware

---

### 3. Hybrid Deployment (.wasm + .aot)

**Use Case:** Production apps, multi-platform support, graceful degradation

```
Upload both app.wasm and app_esp32s3.aot
  ↓
Runtime checks available .aot for current arch
  ↓
Found?  → Load .aot (fast path)
Not found?  → Fall back to .wasm interpreter
```

**Pros:**
- AOT performance on supported architectures
- Universal fallback for unsupported platforms
- Graceful degradation without code changes

**Cons:**
- Requires storing multiple files
- More complex build pipeline
- Higher total storage usage

**Best For:**
- Most serious WAMR deployments
- Production app stores
- Cross-platform SDK development
- Defense-in-depth (compile-time + runtime security)

**Implementation Notes:**
- Runtime auto-detects `.aot` files by naming convention: `app_<arch>.aot`
- Falls back to `app.wasm` if no matching AOT binary found
- No code changes needed—purely deployment-side decision

---

## Using WAMR AOT Compiler (wamrc)

### Installation

```bash
# Clone WAMR repository
git clone https://github.com/bytecodealliance/wasm-micro-runtime.git
cd wasm-micro-runtime/wamr-compiler

# Build wamrc
./build_llvm.sh
mkdir build && cd build
cmake ..
make
sudo cp wamrc /usr/local/bin/
```

### Compilation Examples

#### ESP32-S3 (Xtensa)

```bash
wamrc --target=xtensa \
      --cpu=esp32s3 \
      --size-level=3 \
      -o app_esp32s3.aot \
      app.wasm
```

#### STM32/nRF (ARM Cortex-M)

```bash
wamrc --target=thumbv7 \
      --cpu=cortex-m4 \
      --size-level=3 \
      -o app_stm32.aot \
      app.wasm
```

#### Native Simulation (x86-64)

```bash
wamrc --target=x86-64 \
      --size-level=3 \
      -o app_native.aot \
      app.wasm
```

### Optimization Flags

| Flag | Effect |
|------|--------|
| `--size-level=0` | No optimization (debug) |
| `--size-level=1` | Basic optimization |
| `--size-level=2` | Aggressive optimization |
| `--size-level=3` | Maximum size reduction |
| `--enable-simd` | Enable SIMD instructions |
| `--disable-aux-stack-check` | Remove stack overflow checks (faster, less safe) |

---

## Runtime Loading (Developer View)

**Key Points:**
- WAMR auto-detects file format (magic bytes differ)
- `.wasm` files: `0x00 0x61 0x73 0x6D` (WASM bytecode)
- `.aot` files: Custom WAMR AOT header
- No API changes needed—transparent to application code

---

## Build Pipeline Integration

### Makefile Example

```makefile
# Build universal .wasm
%.wasm: %.c
	$(WASI_SDK)/bin/clang \
		--target=wasm32-wasi \
		-O3 -Wl,--no-entry \
		-o $@ $<

# Compile AOT for ESP32-S3
%.aot: %.wasm
	wamrc --target=xtensa --cpu=esp32s3 --size-level=3 -o $@ $<

# Hybrid deployment (app binaries stream raw to the app-manager route)
deploy: app.wasm app_esp32s3.aot
	curl --data-binary @app.wasm "http://$(DEVICE_IP)/api/apps/install?name=app"
	curl --data-binary @app_esp32s3.aot "http://$(DEVICE_IP)/api/apps/install?name=app"
```

---

## Performance Tuning

### When to Use AOT

**AOT is beneficial:**
- Tight loops (physics, rendering)
- Math-heavy operations
- Frequent function calls
- Real-time constraints (audio, video)
- Battery-powered devices

**Interpreter is sufficient:**
- I/O-bound apps (network, sensor polling)
- Infrequent execution (configuration utilities)
- Simple state machines
- Prototyping and debugging

### Profiling Strategy

1. **Start with interpreter** (.wasm) during development
2. **Profile** with `akira_perf` shell command
3. **Identify bottlenecks** (CPU-bound functions)
4. **Compile AOT** for production
5. **Measure improvement** (should be 10-50x for compute)

---

## Limitations

### AOT-Specific Constraints

1. **Architecture Lock-In**
   - Each `.aot` file works on one CPU architecture only
   - Must maintain separate binaries for multi-platform apps

2. **Storage Overhead**
   - `.aot` files are ~2-3x larger than `.wasm`
   - Hybrid deployment requires both files (3-4x storage)

3. **Debugging Complexity**
   - Native crashes harder to debug than WASM traps
   - Disassembly shows machine code, not WASM opcodes

4. **Compilation Time**
   - AOT compilation adds ~5-30s per module
   - Not suitable for on-device JIT (requires offline compilation)

### WAMR-Specific Notes

- **No JIT:** WAMR does not support JIT (Just-In-Time) compilation—only AOT and interpreter
- **Offline Compilation:** All AOT compilation happens on host PC, not on device
- **Module Caching:** Compiled .aot files can be cached and reused

---

## Best Practices

### Development Workflow

1. **Develop with .wasm** (fast compile, portable testing)
2. **Optimize code** (profiling tools, algorithms)
3. **Compile AOT** for production target
4. **Benchmark** (compare interpreter vs AOT performance)
5. **Deploy hybrid** (AOT + .wasm fallback)

---

## Related Documentation

- [AkiraRuntime Architecture](runtime.md) - Runtime internals
- [Building WASM Apps](../development/building-apps.md) - Compilation workflow
- [Performance Benchmarks](../resources/performance.md) - Real-world metrics
- [Platform Guides](../platform) - Architecture-specific notes

---

## External References

- [WAMR AOT Documentation](https://github.com/bytecodealliance/wasm-micro-runtime/blob/main/doc/build_wamr.md#aot-compiler)
- [wamrc User Guide](https://github.com/bytecodealliance/wasm-micro-runtime/blob/main/wamr-compiler/README.md)
- [WebAssembly Binary Format](https://webassembly.github.io/spec/core/binary/index.html)

---

See [Building Apps Guide](../development/building-apps.md) for the complete build workflow.
