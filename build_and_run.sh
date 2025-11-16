#!/bin/bash
# Complete Build and Run Script for Akira Console Simulator

set -e  # Exit on error

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
WORKSPACE_ROOT="$(dirname "$SCRIPT_DIR")"

echo "🎮 Akira Console Simulator - Complete Build & Run"
echo "=================================================="
echo ""

# Step 1: Build AkiraOS for native_sim
echo "🔨 Building AkiraOS for native_sim..."
echo ""
cd "$WORKSPACE_ROOT"
west build --pristine -b native_sim AkiraOS -d build_native_sim -- -DMODULE_EXT_ROOT="$WORKSPACE_ROOT/AkiraOS"

if [ $? -ne 0 ]; then
    echo "❌ Build failed!"
    exit 1
fi

echo ""
echo "✅ AkiraOS built successfully"
echo ""

# Step 2: Run AkiraOS
echo "🚀 Starting AkiraOS..."
echo ""
cd "$WORKSPACE_ROOT/build_native_sim/zephyr"
./zephyr.exe

echo ""
echo "👋 Done!"

