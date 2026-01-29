#!/bin/bash

# Build script for AkiraOS WASM example applications
# 
# Usage:
#   ./build_wasm_apps.sh              - Build all WASM apps
#   ./build_wasm_apps.sh clean        - Clean all WASM apps
#   ./build_wasm_apps.sh example_app  - Build specific app

set -e

# Configuration
WASI_SDK=${WASI_SDK:-/opt/wasi-sdk}
WASM_APPS_DIR="${0%/*}"
OUTPUT_DIR="${WASM_APPS_DIR}/bin"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Ensure WASI SDK is available
if [ ! -d "$WASI_SDK" ]; then
    echo -e "${RED}Error: WASI SDK not found at $WASI_SDK${NC}"
    echo "Set WASI_SDK environment variable or install from:"
    echo "  https://github.com/WebAssembly/wasi-sdk"
    exit 1
fi

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Function: Build a single WASM app
build_app() {
    local app_name=$1
    local source_file="${WASM_APPS_DIR}/${app_name}.c"
    local output_file="${OUTPUT_DIR}/${app_name}.wasm"

    if [ ! -f "$source_file" ]; then
        echo -e "${YELLOW}Warning: Source file not found: $source_file${NC}"
        return 1
    fi

    echo -e "${GREEN}Building ${app_name}...${NC}"

    # Determine exported functions (look for patterns like "exported function foo()")
    local exports=$(grep -o 'export.*\) *$' "$source_file" 2>/dev/null | \
                   sed 's/export[^a-zA-Z0-9_]*//g' | \
                   sed 's/(.*//' || true)

    # Build export flags
    local export_flags="-Wl,--export=main"
    if [ ! -z "$exports" ]; then
        echo "  Found exports: $exports"
        for func in $exports; do
            export_flags="$export_flags -Wl,--export=$func"
        done
    fi

    # Compile with WASI SDK
    "${WASI_SDK}/bin/clang" \
        -target wasm32-wasi \
        -nostdlib \
        -Wl,--no-entry \
        $export_flags \
        -O2 \
        -g \
        -o "$output_file" \
        "$source_file"

    if [ $? -eq 0 ]; then
        local size=$(stat -c%s "$output_file" 2>/dev/null || stat -f%z "$output_file" 2>/dev/null)
        echo -e "${GREEN}✓ Built: ${app_name}.wasm (${size} bytes)${NC}"
        
        # Show exported functions
        echo "  Exported symbols:"
        "${WASI_SDK}/bin/wasm-nm" "$output_file" | grep -v "^U " | sed 's/^/    /' || true
        
        return 0
    else
        echo -e "${RED}✗ Build failed: ${app_name}${NC}"
        return 1
    fi
}

# Function: Clean build artifacts
clean_apps() {
    echo -e "${YELLOW}Cleaning WASM apps...${NC}"
    rm -rf "$OUTPUT_DIR"
    echo -e "${GREEN}Clean complete${NC}"
}

# Function: List available apps
list_apps() {
    echo -e "${GREEN}Available WASM applications:${NC}"
    for file in "${WASM_APPS_DIR}"/*.c; do
        if [ -f "$file" ]; then
            local name=$(basename "$file" .c)
            echo "  - $name"
        fi
    done
}

# Main script logic
main() {
    local command=${1:-build}

    case "$command" in
        clean)
            clean_apps
            ;;
        list)
            list_apps
            ;;
        all|build)
            echo -e "${GREEN}=== Building AkiraOS WASM Applications ===${NC}"
            echo "WASI SDK: $WASI_SDK"
            echo "Output: $OUTPUT_DIR"
            echo ""
            
            local failed=0
            for file in "${WASM_APPS_DIR}"/*.c; do
                if [ -f "$file" ]; then
                    local name=$(basename "$file" .c)
                    if ! build_app "$name"; then
                        ((failed++))
                    fi
                fi
            done
            
            echo ""
            if [ $failed -eq 0 ]; then
                echo -e "${GREEN}✓ All builds successful${NC}"
                echo ""
                echo "Built WASM apps are in: $OUTPUT_DIR"
                ls -lh "$OUTPUT_DIR"/*.wasm 2>/dev/null || true
                return 0
            else
                echo -e "${RED}✗ ${failed} build(s) failed${NC}"
                return 1
            fi
            ;;
        *)
            # Assume it's a specific app name
            echo -e "${GREEN}=== Building WASM Application ===${NC}"
            if build_app "$command"; then
                return 0
            else
                echo ""
                echo "Usage: $0 [clean|list|build|APP_NAME]"
                echo ""
                list_apps
                return 1
            fi
            ;;
    esac
}

main "$@"
