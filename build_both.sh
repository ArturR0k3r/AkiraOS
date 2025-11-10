#!/bin/bash

# Build script for MCUboot + AkiraOS application
# Usage: ./build_both.sh [platform] [clean]
# Platforms: esp32s3 (default), esp32, esp32c3
# Example: ./build_both.sh esp32s3 clean

set -e  # Exit on any error

WORKSPACE_ROOT="/home/artur_ubuntu/Akira"
PLATFORM="${1:-esp32s3}"
CLEAN_ARG="$2"

# Map platform to board
case "$PLATFORM" in
    esp32s3)
        BOARD="esp32s3_devkitm/esp32s3/procpu"
        PLATFORM_NAME="ESP32-S3 (Akira Console)"
        ;;
    esp32)
        BOARD="esp32_devkitc/esp32/procpu"
        PLATFORM_NAME="ESP32 (Akira Console - Legacy)"
        ;;
    esp32c3)
        BOARD="esp32c3_devkitm"
        PLATFORM_NAME="ESP32-C3 (Akira Modules Only)"
        ;;
    clean)
        # If first arg is "clean", use default platform
        PLATFORM="esp32s3"
        BOARD="esp32s3_devkitm/esp32s3/procpu"
        PLATFORM_NAME="ESP32-S3 (Akira Console)"
        CLEAN_ARG="clean"
        ;;
    help|--help|-h)
        echo "Usage: $0 [platform] [clean]"
        echo ""
        echo "Platforms:"
        echo "  esp32s3  - ESP32-S3 (Akira Console - Primary) [default]"
        echo "  esp32    - ESP32 (Akira Console - Legacy)"
        echo "  esp32c3  - ESP32-C3 (Akira Modules Only)"
        echo ""
        echo "Options:"
        echo "  clean    - Clean build directories before building"
        echo ""
        echo "Examples:"
        echo "  $0                 # Build ESP32-S3"
        echo "  $0 esp32s3 clean   # Clean and build ESP32-S3"
        echo "  $0 esp32c3         # Build ESP32-C3 for Akira Modules"
        echo ""
        echo "Note: ESP32-C3 is for Akira Modules only, not Akira Console!"
        exit 0
        ;;
    *)
        echo "Error: Unknown platform '$PLATFORM'"
        echo "Valid platforms: esp32s3, esp32, esp32c3"
        echo "Use '$0 help' for more information"
        exit 1
        ;;
esac

cd "$WORKSPACE_ROOT"

echo "=========================================="
echo "Building MCUboot and AkiraOS"
echo "Platform: $PLATFORM_NAME"
echo "Board: $BOARD"
echo "=========================================="

# Clean builds if requested
if [ "$CLEAN_ARG" = "clean" ]; then
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