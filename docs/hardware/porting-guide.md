---
layout: default
title: Porting Guide
parent: Hardware
nav_order: 3
permalink: /hardware/porting-guide
---

# AkiraOS BSP Porting Guide
{: .no_toc }

End-to-end instructions for bringing AkiraOS to new hardware in one week.
{: .fs-5 .fw-300 }

---

## Table of Contents
{: .no_toc .text-delta }

1. TOC
{:toc}

---

## Prerequisites

| Tool | Minimum version | Install |
|------|----------------|---------|
| [west](https://docs.zephyrproject.org/latest/develop/west/index.html) | 1.2.0 | `pip install west` |
| Zephyr SDK | 0.16.8 | [SDK releases](https://github.com/zephyrproject-rtos/sdk-ng/releases) |
| CMake | 3.20 | via system package manager |
| Python | 3.10 | via system package manager |
| AkiraSDK | same minor as AkiraOS | included as git submodule (`AkiraSDK/`) |

Initialize the workspace once:

```bash
git clone https://github.com/your-org/AkiraOS.git
cd AkiraOS
west init -l .
cd ..
west update
```

---

## BSP Files

### Directory structure

Copy the template scaffold and rename every file, replacing `template` with
your board identifier and `FIXME_SOC` with the Zephyr SoC name:

```
boards/
├── <board-name>/                         ← board definition directory
│   ├── board.yml
│   ├── <board-name>_<soc>_procpu.yaml
│   ├── Kconfig.<board-name>
│   ├── board.cmake
│   ├── <board-name>-pinctrl.dtsi
│   ├── <board-name>_<soc>_procpu.dts
│   └── <board-name>_<soc>_procpu_defconfig
└── <board-name>_<soc>_procpu.overlay     ← application overlay (flat, next to other boards)
    <board-name>_<soc>_procpu.conf        ← application conf  (flat)
```

### board.yml

Defines the board identity for the Zephyr meta-tool layer.  The `vendor` field
must match an entry in `zephyr/boards/vendor-prefixes.txt`.  If your company is
not listed, add a line to the repo copy at the root of this workspace.

```yaml
board:
  name: my_sensor_node        # lowercase, hyphens OK
  full_name: "MySensorNode (nRF54L15-QFAA-R7)"
  vendor: mycompany
  socs:
    - name: nrf54l15          # must match zephyr/soc/ subdirectory name
```

### .yaml — board target

```yaml
identifier: my_sensor_node/nrf54l15/cpuapp
name: MySensorNode CPUAPP
type: mcu
arch: arm                     # xtensa | arm | riscv
toolchain:
  - zephyr
supported:
  - gpio
  - uart
  - i2c
  - spi
  - watchdog
  - entropy
  - bluetooth                 # remove if absent
vendor: mycompany
```

### Kconfig.\<board-name\>

```kconfig
config BOARD_MY_SENSOR_NODE
    select SOC_NRF54L15_CPUAPP
```

Browse `zephyr/soc/` for the correct `SOC_*` symbol.  For ESP32-S3 modules the
symbol is `SOC_ESP32S3_MINI_N8` regardless of flash/PSRAM variant (those are
set via `CONFIG_ESPTOOLPY_FLASHSIZE_*` and `CONFIG_ESP_SPIRAM_SIZE`).

### board.cmake

Uncomment the runner block matching your debug probe (OpenOCD / J-Link /
esptool).  For J-Link set `--device` to the exact string shown in J-Link
Commander's `ShowDevices` output.  For esptool adjust `--esp-flash-size` and
`--esp-app-address` to match your slot0_partition offset.

### \<board-name\>-pinctrl.dtsi

Map every peripheral signal to a physical GPIO number.  CS lines are **not**
listed here — they go in `cs-gpios` inside the `&spiN` node.

Include the correct SoC pinctrl header:

| SoC family | Header |
|-----------|--------|
| ESP32-S3 | `<dt-bindings/pinctrl/esp32s3-pinctrl.h>` |
| nRF54L / nRF52 / nRF53 | `<zephyr/dt-bindings/pinctrl/nrf-pinctrl.h>` |
| STM32 | `<zephyr/dt-bindings/pinctrl/stm32-pinctrl.h>` |

### \<board-name\>\_\<soc\>\_procpu.dts

1. `#include "<your-soc>.dtsi"` — provides the node skeleton.
2. Optionally `#include` an MCUboot partition DTSI (ESP32 provides one in the
   Zephyr tree; other SoCs define partitions inline).
3. Set `model`, `compatible`, `chosen`, `aliases`.
4. Enable GPIO controllers.
5. Wire UART/SPI/I2C with pinctrl references.
6. Define buttons, LEDs, and any board-specific sensor nodes.
7. Add the flash partition table (see [Flash sizing guide](#flash-sizing-guide)).

### \<board-name\>\_\<soc\>\_procpu\_defconfig

Only put symbols here that are unconditionally required regardless of
application config.  Keep this minimal:

```kconfig
CONFIG_CONSOLE=y
CONFIG_SERIAL=y
CONFIG_UART_CONSOLE=y
CONFIG_GPIO=y
CONFIG_CLOCK_CONTROL=y
```

### First build smoke-test

```bash
cd ..
unset ZEPHYR_BASE
west build --pristine -b my_sensor_node/nrf54l15/cpuapp AkiraOS -d build
```

Expected: CMake configure succeeds and `build/zephyr/zephyr.elf` is produced.
If Kconfig reports `SOC_FIXME undefined`, your `Kconfig.<board>` select is wrong.

---

## Boot and Shell

Enable UART in the overlay and confirm UART0 pinctrl matches your schematic:

```dts
&uart0 { status = "okay"; current-speed = <115200>; };
```

Flash and connect a serial terminal at 115200 8N1.  Expected banner:

```
*** Booting Zephyr OS build v4.3.0 ***
AkiraOS v1.6.5
AkiraOS:~$
```

If the banner doesn't appear:
- Check TX/RX are not swapped (`uart0_default` in pinctrl DTSI).
- Verify `CONFIG_UART_CONSOLE=y` and `CONFIG_SHELL_UART_BACKEND=y` in your conf.
- Try reducing baud to 9600 and increasing with `current-speed`.

---

## WASM Runtime

Minimum conf to enable the WASM runtime:

```kconfig
CONFIG_AKIRA_WASM_RUNTIME=y
CONFIG_WAMR_HEAP_SIZE=131072          # 128 KB starting point
CONFIG_AKIRA_WASM_APP_STACK_SIZE=8192 # 8 KB per app thread
CONFIG_MAX_CONTAINERS=2
```

Verify the runtime initializes:

```
AkiraOS:~$ akira runtime status
WAMR runtime: ready
Heap:   128 KB available
Slots:  0/2 running
```

If you see a stack overflow: increase `CONFIG_AKIRA_WASM_APP_STACK_SIZE`.  
If the system crashes at boot with `heap exhausted`: reduce `CONFIG_WAMR_HEAP_SIZE`
or add PSRAM (see [PSRAM configuration](#psram-configuration)).

---

## First WASM App

Build the hello-world app from AkiraSDK:

```bash
# From the AkiraSDK directory
cd AkiraOS/AkiraSDK
wasi-sdk/bin/clang --target=wasm32-wasi \
  -nostdlib -Wl,--no-entry -Wl,--export-all \
  -I include \
  wasm_apps/hello/main.c -o hello.wasm
```

Copy `hello.wasm` to the LittleFS filesystem via USB mass-storage or the
`akira fs` shell commands, then run:

```
AkiraOS:~$ akira app install /lfs/hello.wasm
AkiraOS:~$ akira app run hello
Hello from WASM!
```

---

## Flash Partitions and OTA

### Flash sizing guide

Calculate your partition layout from the bottom up:

| Partition | Minimum | Recommended | Notes |
|-----------|---------|-------------|-------|
| MCUboot (`boot`) | 64 KB | 128 KB | Must be read-only |
| App (`slot0`) | 256 KB | 1 MB | Depends on feature set enabled |
| OTA staging (`slot1`) | = slot0 | = slot0 | Must equal slot0 for swap-based OTA |
| NVS settings | 32 KB (2 erase blocks) | 32 KB | BT bonds, Wi-Fi credentials |
| LittleFS | 256 KB | 512 KB | WASM app storage |

Minimum viable total: **512 KB** (no OTA, no LittleFS, minimal app).  
Standard: **4 MB** (OTA + 512 KB data).  
Full: **8–16 MB** (multiple large WASM apps on SD or LittleFS).

In your overlay:

```dts
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells    = <1>;

        // Offsets MUST match boot_partition end + slot0_partition end.
        akira_settings_nvs_partition: partition@220000 {
            label = "akira_settings_nvs";
            reg   = <0x00220000 DT_SIZE_K(32)>;
        };
        lfs1_partition: partition@228000 {
            label = "lfs_storage";
            reg   = <0x00228000 DT_SIZE_K(256)>;
        };
    };
};
```

Verify offsets with:

```
AkiraOS:~$ akira storage list
```

### OTA

```kconfig
CONFIG_MCUBOOT=y
CONFIG_AKIRA_OTA=y
CONFIG_BOOTLOADER_MCUBOOT=y
```

Run a loopback OTA test:

```
AkiraOS:~$ akira ota status
slot0: active  (0x020000 – 0x11FFFF)
slot1: empty   (0x120000 – 0x21FFFF)
```

---

## Validation

### Platform HAL extension

If your SoC is not in the existing list, add a detection block to
[`src/drivers/platform_hal.h`](https://github.com/ArturR0k3r/AkiraOS/blob/main/src/drivers/platform_hal.h):

```c
#elif defined(CONFIG_SOC_MY_SOC)
#define AKIRA_PLATFORM_NATIVE_SIM 0
#define AKIRA_PLATFORM_ESP32      0
#define AKIRA_PLATFORM_ESP32S3    0
#define AKIRA_PLATFORM_ESP32C6    0
#define AKIRA_PLATFORM_ESP32H2    0
#define AKIRA_PLATFORM_STM32      0
#define AKIRA_PLATFORM_NORDIC     0
#define AKIRA_PLATFORM_MY_BOARD   1   // add this flag
```

Then update `platform_hal.c` to handle `AKIRA_PLATFORM_MY_BOARD` for any
platform-specific initialization it performs (clock setup, power domains, etc.).

### Custom driver registration

Register hardware-specific drivers at init time using the driver registry:

```c
#include "drivers/driver_registry.h"

static const driver_ops_t my_imu_ops = {
    .init  = my_imu_init,
    .read  = my_imu_read,
    .ioctl = my_imu_ioctl,
};

// Called from your board init or SYS_INIT:
driver_registry_register("my_imu", DRIVER_TYPE_SENSOR_IMU, &my_imu_ops);
```

WASM apps retrieve the driver by name via `akira_sensor_read()` in AkiraSDK.

### PSRAM configuration

PSRAM is required to run more than 2 WASM apps concurrently (see
the minimum hardware requirements in [the docs home](../index.md)).

ESP32-S3 with OPI PSRAM (N8R8 / N16R8 module):

```kconfig
CONFIG_ESP_SPIRAM=y
CONFIG_ESP_SPIRAM_SIZE=8388608          # 8 MB
CONFIG_ESP_SPIRAM_HEAP_SIZE=4194304     # 4 MB for WAMR heap
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_MEMC=y
CONFIG_SHARED_MULTI_HEAP=y
CONFIG_WAMR_HEAP_SIZE=1048576          # 1 MB — draws from PSRAM heap
```

In the DTS overlay:

```dts
&psram0 {
    size   = <DT_SIZE_M(8)>;
    status = "okay";
};
```

### WASM runtime tuning

| Symbol | Default | Description |
|--------|---------|-------------|
| `CONFIG_WAMR_HEAP_SIZE` | 262144 | Soft quota hint for WAMR allocator (bytes) |
| `CONFIG_AKIRA_WASM_APP_STACK_SIZE` | 8192 | Stack per WASM app thread (bytes) |
| `CONFIG_AKIRA_WASM_APP_PRIORITY` | 14 | Zephyr thread priority for WASM threads |
| `CONFIG_MAX_CONTAINERS` | 8 | Maximum simultaneous running WASM apps |
| `CONFIG_WAMR_AOT_SUPPORT` | n | Enable AOT compilation (requires toolchain) |

Enable AOT for production to eliminate interpreter overhead:
`CONFIG_WAMR_AOT_SUPPORT=y` (ESP32-S3 and native_sim only in current release).

---

## Go/No-Go Checklist

| # | Test | Command / Action | Pass criteria |
|---|------|-----------------|---------------|
| 1 | Build | `west build -b <board>` | Zero errors |
| 2 | Boot | Power cycle | Shell prompt within 3 s |
| 3 | Watchdog | `akira wdt test` | Board resets and recovers |
| 4 | UART | Terminal at 115200 | Characters echo correctly |
| 5 | Flash | `akira storage list` | Correct offsets reported |
| 6 | LittleFS | `akira fs ls /lfs` | Lists directory without error |
| 7 | NVS | `akira settings set test_key hello` then reboot | Value persists |
| 8 | WASM runtime | `akira runtime status` | `ready` |
| 9 | WASM app install | `akira app install /lfs/hello.wasm` | No error |
| 10 | WASM app run | `akira app run hello` | Expected output printed |
| 11 | Display (if fitted) | `akira display clear 0xF800` | Screen turns red |
| 12 | OTA (if enabled) | `akira ota status` | Correct slot addresses |
| 13 | BLE (if enabled) | `akira ble scan` | RSSI readings appear |
| 14 | Sanitizer | `west build -b native_sim` | All 54 ztests pass |

---

## Common Errors

| Error message | Likely cause | Fix |
|---------------|-------------|-----|
| `__device_dts_ord_N undeclared` | cs-gpios phandle points to disabled node | Redirect CS GPIO to a direct GPIO pin; see `boards/akiraconsole_prod_esp32s3_procpu.overlay` comments |
| `SOC_FIXME undefined` | Kconfig symbol not found | Check `Kconfig.<board>` select value against `zephyr/soc/` tree |
| `heap exhausted` at boot | WAMR heap > available SRAM | Reduce `CONFIG_WAMR_HEAP_SIZE` or add PSRAM |
| `pinmux <FIXME>` does not compile | SoC pinctrl header not included | Add correct `#include` in pinctrl DTSI |
| Stack overflow in app thread | `AKIRA_WASM_APP_STACK_SIZE` too small | Increase by 4 KB increments until stable |
| Display stays black | Wrong DC/RST GPIO or wrong polarity | Compare `dc-gpios` / `reset-gpios` against schematic |

---

## Per-SoC notes

AkiraOS supports boards across four SoC families out of the box; the reference
board confs in `snippets/akira-board/boards/` are worked examples. Family
specifics when porting your own board:

### Espressif ESP32-S3 / C3 / C6 / H2

- The flagship path. ESP32-S3 gets PSRAM via `CONFIG_ESP_SPIRAM` +
  `CONFIG_AKIRA_PSRAM`; put the WAMR heap, framebuffer and bulk statics there —
  internal DRAM is the bottleneck (BT/WiFi/WASM stacks must stay in SRAM).
- WiFi is `CONFIG_WIFI_ESP32` + `CONFIG_NET_SOCKETS_OFFLOAD`; pair the board with
  `akira-profile-connected`.
- MCUboot on ESP32-C6/H2 needs AkiraOS's patched linker script — `build.sh`
  passes it via `CONFIG_CUSTOM_LINKER_SCRIPT` (the Zephyr tree is never edited).
- RISC-V ESP32 (C6/H2) images are RISC-V; watch the BLE system-heap floor on S3
  (see the BT heap notes in the architecture docs).

### Nordic nRF54L15 (and nRF5x)

- BLE-first, no WiFi: pair with `akira-profile-sensor-node` or `minimal` plus
  `CONFIG_BT`. Example: `nrf54l15dk/nrf54l15/cpuapp`.
- No PSRAM — keep `CONFIG_WAMR_HEAP_SIZE` and `MAX_CONTAINERS` small; the WASM
  runtime allocates from the system heap.
- Flash is internal RRAM/flash; MCUboot uses the in-tree partitions.

### STMicroelectronics STM32 (H7 / L4 / U5)

- Networked STM32 (nucleo_h743zi) uses the on-chip MAC + an external PHY, not
  WiFi. Sensor/low-power STM32 (nucleo_l476rg, b_u585i_iot02a, steval_stwinbx1)
  run headless with `akira-profile-sensor-node`.
- **Signing:** STM32 boards build signed MCUboot images. Do **not** ship with
  MCUboot's development key — generate a product key (`west akira keygen`) and
  set `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE`; `CONFIG_AKIRA_RELEASE_BUILD=y` fails
  the build if the dev key is still in use.
- No PSRAM; size the WAMR heap to internal SRAM.

### Raspberry Pi RP2040 / RP2350 (Pico, Pico 2)

- The shell is over **USB CDC-ACM** (`CONFIG_AKIRA_USB` + USB CDC), not UART —
  the standard Pico exposes no UART header. Confirm the CDC console in the board
  overlay.
- No wireless on the base Pico; a headless `minimal`/`sensor-node` product. Small
  SRAM — keep the WASM heap and container count modest.
- RP2350 (`rpi_pico2/rp2350a/m33`) is Cortex-M33; RP2040 is dual Cortex-M0+.

---

## References

- [AkiraOS Architecture](../architecture/)
- [Hardware Index](index.md)
- [AkiraSDK API Reference](../api-reference/)
- [Zephyr Board Porting Guide](https://docs.zephyrproject.org/latest/hardware/porting/board_porting.html)
- [Zephyr Device Tree Overview](https://docs.zephyrproject.org/latest/build/dts/index.html)
- [Template BSP scaffold](https://github.com/ArturR0k3r/AkiraOS/tree/main/boards/template) — start here

---

*Last updated: 2026-09-14 (AkiraOS v1.6.5)*
