#!/bin/bash
# Complete Build and Run Script for Akira Console Simulator

set -e  # Exit on error

echo "🎮 Akira Console Simulator - Complete Build & Run"
echo "=================================================="
echo ""

# Step 1: Build SDL2 Viewer
if [ ! -f "tools/akira_viewer" ]; then
    echo "📦 Building SDL2 viewer..."
    cd tools
    make
    cd ..
    echo "✅ SDL2 viewer built"
    echo ""
else
    echo "✅ SDL2 viewer already built"
    echo ""
fi

# Step 2: Build AkiraOS for native_sim
echo "🔨 Building AkiraOS for native_sim..."
echo ""
west build --pristine -b native_sim . -d ../build_native_sim

if [ $? -ne 0 ]; then
    echo "❌ Build failed!"
    exit 1
fi

echo ""
echo "✅ AkiraOS built successfully"
echo ""

# Step 3: Start SDL2 viewer in background
echo "📺 Starting SDL2 simulator window..."
./tools/akira_viewer &
VIEWER_PID=$!

# Wait for viewer to initialize
sleep 1

# Step 4: Run AkiraOS
echo "🚀 Starting AkiraOS..."
echo ""
cd ../build_native_sim/zephyr
./zephyr.exe

# Cleanup
echo ""
echo "🧹 Cleaning up..."
kill $VIEWER_PID 2>/dev/null
wait $VIEWER_PID 2>/dev/null

echo "👋 Done!"
