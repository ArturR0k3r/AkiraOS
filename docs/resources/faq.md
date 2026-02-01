# Frequently Asked Questions

## General

**Q: What is AkiraOS?**  
A: AkiraOS is an embedded operating system combining Zephyr RTOS with WebAssembly for secure, dynamic application execution on resource-constrained devices.

**Q: What platforms are supported?**  
A: ESP32-S3 (primary), nRF54L15, STM32 (experimental), and native simulation.

**Q: Is AkiraOS production-ready?**  
A: Yes, v1.4.7 is stable for production use on ESP32-S3.

---

## WebAssembly

**Q: Can I use existing WASM modules?**  
A: Only if they don't rely on WASI or browser APIs. AkiraOS uses custom native APIs.

**Q: What languages can I use?**  
A: Any language that compiles to WASM: C, C++, Rust, AssemblyScript, TinyGo.

**Q: What's the performance overhead?**  
A: Native call overhead is ~60ns. WASM is 1.5-3x slower than native code.

---

## Development

**Q: How do I get started?**  
A: Follow the [Installation Guide](../getting-started/installation.md).

**Q: Can I debug WASM apps?**  
A: Limited. Use logging and native sim for logic testing.

**Q: How do I update firmware?**  
A: Via OTA over WiFi or USB flashing. See [OTA Guide](../development/ota-updates.md).

---

## Hardware

**Q: What's the minimum RAM required?**  
A: 256KB, but 512KB+ recommended for multiple apps.

**Q: Do I need PSRAM?**  
A: Highly recommended for running multiple WASM apps (4+MB).

**Q: Can I use custom hardware?**  
A: Yes, if Zephyr supports it. See [Porting Guide](../development/porting.md).

---

## Security

**Q: How secure are WASM apps?**  
A: WASM provides memory isolation. Capabilities provide access control. See [Security Model](../architecture/security.md).

**Q: Can apps access each other's data?**  
A: No, WASM memory is isolated. File system access is restricted per-app.

**Q: Is OTA secure?**  
A: Yes, MCUboot verifies firmware signatures and provides rollback protection.

---

## Troubleshooting

**Q: Build fails with "west not found"**  
A: Install west: `pip3 install --user west`

**Q: Flash fails with "Permission denied"**  
A: Add user to dialout group: `sudo usermod -a -G dialout $USER`

**Q: WiFi won't connect**  
A: Check SSID/password in board config. Ensure 2.4GHz network.

See [Troubleshooting Guide](../getting-started/troubleshooting.md) for more.

---

## Community

**Q: How do I contribute?**  
A: See [CONTRIBUTING.md](../../CONTRIBUTING.md).

**Q: Where can I get help?**  
A: GitHub Discussions, Issues, or Zephyr Discord.

**Q: Is there commercial support?**  
A: Contact the project maintainers for commercial inquiries.
