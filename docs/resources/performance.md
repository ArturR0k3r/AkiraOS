# Performance Benchmarks

Performance metrics for AkiraOS on ESP32-S3.

## System Performance

| Metric | Value | Platform |
|--------|-------|----------|
| Boot Time | ~500ms | ESP32-S3 @ 240MHz |
| Shell Response | <10ms | All platforms |
| Thread Switch | <5μs | Zephyr scheduler |

---

## Runtime Performance

| Operation | Time | Notes |
|-----------|------|-------|
| **WASM Load (100KB)** | ~80ms | Chunked loading |
| **Native Call Overhead** | ~60ns | Inline cap check |
| **App Start** | ~50ms | Instantiation + init |
| **App Switch** | <1ms | Context save/restore |

---

## Network Performance

| Operation | Throughput | Latency |
|-----------|------------|---------|
| **HTTP Upload** | ~1.3 MB/s | N/A |
| **OTA Flash Write** | ~200 KB/s | 10-20ms per block |
| **BLE Transfer** | ~10 KB/s | <10ms |
| **WiFi Scan** | ~2s | 13 channels |

---

## Memory Usage

| Component | SRAM | PSRAM |
|-----------|------|-------|
| **Kernel** | ~128KB | - |
| **Network Stack** | ~32KB | 12KB buffers |
| **WASM Pool** | - | 256KB |
| **Per App (avg)** | ~4KB | 64-128KB |

---

## Power Consumption (ESP32-S3)

| Mode | Current | Use Case |
|------|---------|----------|
| Active (WiFi TX) | ~160mA | File upload |
| Active (idle) | ~80mA | App running |
| Modem sleep | ~30mA | WiFi power save |
| Light sleep | ~3mA | CPU halted |

---

## Optimization Tips

1. **Enable AOT Compilation** - 2x performance boost
2. **Optimize WASM** - Use `wasm-opt -Oz`
3. **Reduce Logging** - Set to WARNING in production
4. **Use Inline Functions** - Reduce call overhead
5. **Batch Operations** - Minimize native calls

---

## Comparison

### AkiraOS vs Native Code

| Metric | Native | WASM (Interp) | WASM (AOT) |
|--------|--------|---------------|------------|
| Execution Speed | 1x | 0.3-0.5x | 0.6-0.8x |
| Memory Overhead | - | +32KB runtime | +32KB runtime |
| Safety | ⚠️ Unsafe | ✅ Sandboxed | ✅ Sandboxed |

---

## Benchmarking Tools

**Custom Benchmarks:**
```bash
uart:~$ benchmark native_call
uart:~$ benchmark wasm_load
```

**System Profiling:**
```bash
uart:~$ kernel threads
uart:~$ kernel stacks
uart:~$ wasm status
```

---

## Related Documentation

- [Architecture Overview](../architecture/)
- [Runtime Performance](../architecture/runtime.md)
- [Data Flow](../architecture/data-flow.md)
