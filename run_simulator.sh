#!/bin/bash
# Run Akira Console Simulator with Visual Window

echo "🎮 Akira Console Simulator"
echo "=========================="
echo ""

# Check if akira_viewer exists
if [ ! -f "tools/akira_viewer" ]; then
    echo "📦 Building SDL2 viewer..."
    cd tools
    make
    cd ..
    echo ""
fi

# Start SDL2 viewer in background
echo "📺 Starting SDL2 simulator window..."
./tools/akira_viewer &
VIEWER_PID=$!

# Wait for viewer to initialize
sleep 1

# Run AkiraOS
echo "🚀 Starting AkiraOS..."
echo ""
cd build_native_sim/zephyr
./zephyr.exe

# Cleanup
echo ""
echo "🧹 Cleaning up..."
kill $VIEWER_PID 2>/dev/null
wait $VIEWER_PID 2>/dev/null

echo "👋 Done!"
