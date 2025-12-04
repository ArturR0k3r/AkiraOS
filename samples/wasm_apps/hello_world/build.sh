#!/bin/sh
# @copyright Copyright © contributors to Project Ocre,
# which has been established as Project Ocre a Series of LF Projects, LLC
# SPDX-License-Identifier: Apache-2.0
#
# Build script for AkiraOS WASM apps - follows OCRE SDK patterns

set -e

# Ensure build directory exists
mkdir -p build
cd build

# Run cmake if no makefile
if [ ! -f Makefile ]; then
    cmake ..
fi

# Build
make

# Show results
echo ""
echo "=== Build Results ==="
ls -la *.wasm

# Show memory info if wasm-objdump is available
if command -v wasm-objdump >/dev/null 2>&1; then
    echo ""
    echo "=== WASM Memory Info ==="
    wasm-objdump -x *.wasm | grep -A1 Memory || true
    wasm-objdump -x *.wasm | grep "global\[0\]" || true
fi
