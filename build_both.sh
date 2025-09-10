#!/bin/bash

# Build script for MCUboot + AkiraOS application
# Usage: ./build_both.sh [clean]

set -e  # Exit on any error

WORKSPACE_ROOT="/home/artur_ubuntu/Akira"
BOARD="esp32_devkitc/esp32/procpu"

cd "$WORKSPACE_ROOT"

echo "=========================================="
echo "Building MCUboot and AkiraOS for ESP32"
echo "=========================================="

# Clean builds if requested
if [ "$1" = "clean" ]; then
    echo "Cleaning existing builds..."
    rm -rf build-mcuboot
    rm -rf build
    rm -rf bootloader/mcuboot/boot/zephyr/build
fi

# Build MCUboot (Bootloader)
echo ""
echo "Building MCUboot bootloader..."
echo "=========================================="
west build -b "$BOARD" bootloader/mcuboot/boot/zephyr -d build-mcuboot

if [ $? -eq 0 ]; then
    echo "✅ MCUboot build successful!"
    echo "MCUboot binary: build-mcuboot/zephyr/zephyr.bin"
else
    echo "❌ MCUboot build failed!"
    exit 1
fi

# Build AkiraOS Application
echo ""
echo "Building AkiraOS application..."
echo "=========================================="
unset ZEPHYR_BASE
west build --pristine -b "$BOARD" AkiraOS -d build -- -DMODULE_EXT_ROOT="$WORKSPACE_ROOT/AkiraOS"

if [ $? -eq 0 ]; then
    echo "✅ AkiraOS build successful!"
    echo "Application binary: build/zephyr/zephyr.bin"
else
    echo "❌ AkiraOS build failed!"
    exit 1
fi

echo ""
echo "=========================================="
echo "Build Summary:"
echo "=========================================="
echo "MCUboot bootloader:  build-mcuboot/zephyr/zephyr.bin"
echo "AkiraOS application: build/zephyr/zephyr.bin"
echo ""
echo "Flash commands:"
echo "1. Flash MCUboot:    esptool write-flash 0x1000 build-mcuboot/zephyr/zephyr.bin"
echo "2. Flash AkiraOS:    esptool write-flash 0x20000 build/zephyr/zephyr.signed.bin"
echo "   (or use: west flash -d build)"
echo ""
echo "🎉 All builds completed successfully!"