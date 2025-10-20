#!/bin/bash
# AkiraOS Multi-Platform Build Script
# Builds for native_sim, ESP32-S3, and ESP32 (original)

set -e  # Exit on error

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "========================================"
echo "  AkiraOS Multi-Platform Build Script"
echo "========================================"
echo ""

# Function to build for a specific platform
build_platform() {
    local platform=$1
    local board=$2
    local build_dir=$3
    
    echo "----------------------------------------"
    echo "Building for $platform ($board)"
    echo "----------------------------------------"
    
    cd "$PROJECT_ROOT/.."
    
    if west build --pristine -b "$board" AkiraOS -d "$build_dir"; then
        echo "✅ $platform build SUCCESS"
        
        # Show memory usage for ESP32 builds
        if [[ "$platform" != "native_sim" ]]; then
            echo ""
            echo "Memory usage:"
            grep -A 15 "Memory region" "$build_dir/build.log" 2>/dev/null || \
            grep -A 15 "Memory region" "$build_dir/zephyr/.config" 2>/dev/null || \
            echo "  (Memory report not available)"
        fi
        
        return 0
    else
        echo "❌ $platform build FAILED"
        return 1
    fi
}

# Track successes and failures
declare -A results

# Build native_sim
if build_platform "native_sim" "native_sim" "build_native_sim"; then
    results[native_sim]="✅"
else
    results[native_sim]="❌"
fi
echo ""

# Build ESP32-S3
if build_platform "ESP32-S3" "esp32s3_devkitm/esp32s3/procpu" "build_esp32s3"; then
    results[esp32s3]="✅"
else
    results[esp32s3]="❌"
fi
echo ""

# Build ESP32 (original)
if build_platform "ESP32" "esp32_devkitc/esp32/procpu" "build_esp32"; then
    results[esp32]="✅"
else
    results[esp32]="❌"
fi
echo ""

# Summary
echo "========================================"
echo "  Build Summary"
echo "========================================"
echo "native_sim:  ${results[native_sim]}"
echo "ESP32-S3:    ${results[esp32s3]}"
echo "ESP32:       ${results[esp32]}"
echo ""

# Check if all succeeded
if [[ "${results[native_sim]}" == "✅" && "${results[esp32s3]}" == "✅" && "${results[esp32]}" == "✅" ]]; then
    echo "🎉 All platforms built successfully!"
    echo ""
    echo "Next steps:"
    echo "  • Run native_sim:  ./build_native_sim/zephyr/zephyr.exe"
    echo "  • Flash ESP32-S3:  west flash -d build_esp32s3"
    echo "  • Flash ESP32:     west flash -d build_esp32"
    exit 0
else
    echo "⚠️  Some builds failed. Please check the output above."
    exit 1
fi
