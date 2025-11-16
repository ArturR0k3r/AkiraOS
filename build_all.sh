#!/bin/bash
# AkiraOS Multi-Platform Build Script
# Usage: ./build_all.sh [platform]
# Platforms: all, native_sim, esp32s3, esp32, esp32c3
# Example: ./build_all.sh esp32s3

set -e  # Exit on error

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
WORKSPACE_ROOT="$(dirname "$SCRIPT_DIR")"
PLATFORM="${1:-all}"

echo "========================================"
echo "  AkiraOS Multi-Platform Build Script"
echo "========================================"
echo ""

# Show help
if [[ "$PLATFORM" == "help" || "$PLATFORM" == "--help" ]]; then
    echo "Usage: $0 [platform]"
    echo ""
    echo "Platforms:"
    echo "  all        - Build all platforms (default)"
    echo "  native_sim - Native simulator (testing)"
    echo "  esp32s3    - ESP32-S3 (Akira Console - Primary)"
    echo "  esp32      - ESP32 (Akira Console - Legacy)"
    echo "  esp32c3    - ESP32-C3 (Akira Modules Only)"
    echo ""
    echo "Examples:"
    echo "  $0              # Build all platforms"
    echo "  $0 esp32s3      # Build only ESP32-S3"
    echo "  $0 esp32c3      # Build only ESP32-C3"
    exit 0
fi

echo "Building platform: $PLATFORM"
echo ""

# Function to build for a specific platform
build_platform() {
    local platform=$1
    local board=$2
    local build_dir=$3
    
    echo "----------------------------------------"
    echo "Building for $platform ($board)"
    echo "----------------------------------------"
    
    cd "$WORKSPACE_ROOT"
    
    if west build --pristine -b "$board" AkiraOS -d "$build_dir" -- -DMODULE_EXT_ROOT="$WORKSPACE_ROOT/AkiraOS"; then
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
if [[ "$PLATFORM" == "all" || "$PLATFORM" == "native_sim" ]]; then
    if build_platform "native_sim" "native_sim" "build_native_sim"; then
        results[native_sim]="✅"
    else
        results[native_sim]="❌"
    fi
    echo ""
fi

# Build ESP32-S3 (Akira Console - Primary)
if [[ "$PLATFORM" == "all" || "$PLATFORM" == "esp32s3" ]]; then
    if build_platform "ESP32-S3 (Akira Console)" "esp32s3_devkitm/esp32s3/procpu" "build_esp32s3"; then
        results[esp32s3]="✅"
    else
        results[esp32s3]="❌"
    fi
    echo ""
fi

# Build ESP32 (Akira Console - Legacy)
if [[ "$PLATFORM" == "all" || "$PLATFORM" == "esp32" ]]; then
    if build_platform "ESP32 (Akira Console - Legacy)" "esp32_devkitc/esp32/procpu" "build_esp32"; then
        results[esp32]="✅"
    else
        results[esp32]="❌"
    fi
    echo ""
fi

# Build ESP32-C3 (Akira Modules Only)
if [[ "$PLATFORM" == "all" || "$PLATFORM" == "esp32c3" ]]; then
    if build_platform "ESP32-C3 (Akira Modules Only)" "esp32c3_devkitm" "build_esp32c3"; then
        results[esp32c3]="✅"
    else
        results[esp32c3]="❌"
    fi
    echo ""
fi

# Summary
echo "========================================"
echo "  Build Summary"
echo "========================================"
[[ -n "${results[native_sim]}" ]] && echo "native_sim:  ${results[native_sim]}"
[[ -n "${results[esp32s3]}" ]] && echo "ESP32-S3 (Akira Console):    ${results[esp32s3]}"
[[ -n "${results[esp32]}" ]] && echo "ESP32 (Akira Console):       ${results[esp32]}"
[[ -n "${results[esp32c3]}" ]] && echo "ESP32-C3 (Akira Modules):    ${results[esp32c3]}"
echo ""

# Check if all succeeded
all_success=true
for result in "${results[@]}"; do
    if [[ "$result" != "✅" ]]; then
        all_success=false
        break
    fi
done

if $all_success; then
    echo "🎉 All requested platforms built successfully!"
    echo ""
    echo "Next steps:"
    [[ -n "${results[native_sim]}" ]] && echo "  • Run native_sim:    ./build_native_sim/zephyr/zephyr.exe"
    [[ -n "${results[esp32s3]}" ]] && echo "  • Flash ESP32-S3:    west flash -d build_esp32s3"
    [[ -n "${results[esp32]}" ]] && echo "  • Flash ESP32:       west flash -d build_esp32"
    [[ -n "${results[esp32c3]}" ]] && echo "  • Flash ESP32-C3:    west flash -d build_esp32c3"
    echo ""
    echo "Platform Notes:"
    echo "  • ESP32-S3/ESP32: For Akira Console (handheld device)"
    echo "  • ESP32-C3: For Akira Modules only (remote sensors/peripherals)"
    exit 0
else
    echo "⚠️  Some builds failed. Please check the output above."
    exit 1
fi
