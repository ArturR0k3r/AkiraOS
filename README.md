# AkiraOS
Retro gaming console with ESP32, Zephyr OS, and WebAssembly support and CyberSec networking tools 

```c
 █████╗ ██╗  ██╗██╗██████╗  █████╗        ██████╗ ███████╗  
██╔══██╗██║ ██╔╝██║██╔══██╗██╔══██╗      ██╔═══██╗██╔════╝  
███████║█████╔╝ ██║██████╔╝███████║█████╗██║   ██║███████╗  
██╔══██║██╔═██╗ ██║██╔══██╗██╔══██║╚════╝██║   ██║╚════██║  
██║  ██║██║  ██╗██║██║  ██║██║  ██║      ╚██████╔╝███████║  
╚═╝  ╚═╝╚═╝  ╚═╝╚═╝╚═╝  ╚═╝╚═╝  ╚═╝       ╚═════╝ ╚══════╝  
```

🎮 Akira Console
Akira Console is a minimalist retro-cyberpunk game console powered by an ESP32-S3 chip running a custom Zephyr RTOS. It supports applications written in WebAssembly (WASM) and aims to serve as both a portable gaming device and a hacker's toolkit.

![DSC_0078](https://github.com/user-attachments/assets/8e9d29de-1b5c-471f-b80c-44f2f96c4fae)

📌 Project Goals

✅ Build a compact and stylish console with a retro aesthetic

✅ Create a lightweight custom OS based on Zephyr

✅ Support WASM-based games and utilities

✅ Add a Hacker Terminal Mode for network tools and CLI access

⚙️ Tech Specs

🧠 Hardware

MCU: ESP32-S3-WROOM-32 (Wi-Fi + Bluetooth, WASM-capable)

Display: 2.4" TFT SPI (ILI9341, 240×320 resolution)

Power: Li-ion Battery with USB-C TP4056 charging module

Controls: 4 for a standard D-Pad + 4 action buttons 

![DSC_0081](https://github.com/user-attachments/assets/5d010761-cffb-4be3-8abe-2f69cc3b8900)


🖥 Software
OS
Based on Zephyr RTOS — lightweight, low-power, modular

Custom core with clean API for apps

Applications
Written in C/C++/Rust, compiled to WASM

Powered by WAMR(WebAssembly Micro Runtime) and OCRE(Open Containers Runtime Enviroenment)

Simple API framework (graphics, input, sound) for easy development

Hacker Mode Features
Soon...

🎨 Design & UX
Minimalist UI with pixel-art and retro themes

Cyberpunk skins: neon colors, ASCII/CRT visual effects

Terminal-style menu with CRT/glitch animations

🙏 Acknowledgements
Special thanks to:

OCRE Project – for inspiration and open tooling

Zephyr Project – for the powerful RTOS base

WebAssembly – for bringing platform-agnostic apps to embedded systems





# Start-up
You should have installed Zephyr, Python and so on checkout how to install zephyr
https://docs.zephyrproject.org/latest/develop/getting_started/index.html 
it should be done in WSL 


```shell
mkdir Akira 
cd Akira 
git clone https://github.com/ArturR0k3r/AkiraOS.git
cd AkiraOS/
west init -l .  
west update
```


```shell
AkiraOS.code-workspace // file used to open the workspace
```


# Build 
```Shell 
cd < root where AkiraOS is located >

unset ZEPHYR_WASM_MICRO_RUNTIME_KCONFIG && west build -b esp32_devkitc/esp32/procpu bootloader/mcuboot/boot/zephyr -- -DMCUBOOT_LOG_LEVEL=4

unset ZEPHYR_BASE && west build --pristine -b esp32_devkitc/esp32/procpu /home/artur_ubuntu/Akira/AkiraOS -d build -- -DMODULE_EXT_ROOT=/home/artur_ubuntu/Akira/AkiraOS
```
or 
```Shell 
ctrl+shift+B -for application build 
```
or
```Shell 
chmod +x build_both.sh
./build_both.sh clean
```



# Flash
```shell
# 1. Flash MCUboot: 
esptool write-flash 0x1000 build-mcuboot/zephyr/zephyr.bin

# 2. Flash AkiraOS: 
esptool write-flash 0x20000 build/zephyr/zephyr.signed.bin
# (or use: west flash -d build)"
```
or
```Shell 
chmod +x flash.sh
# Flash both
./flash.sh
# Flash only the bootloader:
./flash.sh --bootloader-only
# Flash only the application:
./flash.sh --app-only
```
Ы
# Bootload 
```shell
west build --pristine -b esp32_devkitc_wroom ~/zephyrdemo/bootloader/mcuboot/boot/zephyr -d build-mcuboot -- -DCONFIG_BOOT_MAX_IMG_SECTORS=1024 -DDTC_OVERLAY_FILE="~/zephyrdemo/application/boards/esp32_devkitc_wroom.overlay;~/zephyrdemo/bootloader/mcuboot/boot/zephyr/app.overlay" -DMODULE_EXT_ROOT=../../../../application
```

# Firmware
```shell
west build --pristine -b esp32_devkitc_wroom ~/zephyrdemo/application -d build  -- -DCONFIG_BOOTLOADER_MCUBOOT=y  -DCONFIG_MCUBOOT_SIGNATURE_KEY_FILE=\"bootloader/mcuboot/root-rsa-2048.pem\" -DMODULE_EXT_ROOT=.
```

