#!/bin/bash
#
# build_wasm_app.sh - Build WASM applications for AkiraOS
#
# Usage:
#   ./build_wasm_app.sh [options] <source_files...>
#
# Options:
#   -o <output>      Output filename (default: app.wasm)
#   -O0/-O1/-O2/-O3/-Os  Optimization level (default: -Os)
#   -I <path>        Add include path
#   -D <define>      Add preprocessor definition
#   -v               Verbose output
#   -h               Show help
#
# Examples:
#   ./build_wasm_app.sh main.c
#   ./build_wasm_app.sh -o my_app.wasm -O3 src/*.c
#   ./build_wasm_app.sh -I ./include -D DEBUG main.c utils.c

set -e

# Default values
OUTPUT="app.wasm"
OPT_LEVEL="-Os"
INCLUDES=""
DEFINES=""
VERBOSE=0
SOURCES=()

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Find WASI SDK
find_wasi_sdk() {
    if [ -n "$WASI_SDK_PATH" ] && [ -d "$WASI_SDK_PATH" ]; then
        echo "$WASI_SDK_PATH"
        return 0
    fi
    
    # Common installation paths
    local paths=(
        "/opt/wasi-sdk"
        "$HOME/wasi-sdk"
        "/usr/local/wasi-sdk"
        "$HOME/.local/wasi-sdk"
    )
    
    for path in "${paths[@]}"; do
        if [ -d "$path" ] && [ -x "$path/bin/clang" ]; then
            echo "$path"
            return 0
        fi
    done
    
    return 1
}

# Print usage
usage() {
    echo "Usage: $0 [options] <source_files...>"
    echo ""
    echo "Build WASM applications for AkiraOS"
    echo ""
    echo "Options:"
    echo "  -o <output>      Output filename (default: app.wasm)"
    echo "  -O0/-O1/-O2/-O3  Optimization level"
    echo "  -Os              Optimize for size (default)"
    echo "  -I <path>        Add include path"
    echo "  -D <define>      Add preprocessor definition"
    echo "  -v               Verbose output"
    echo "  -h               Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 main.c"
    echo "  $0 -o my_app.wasm -O3 src/*.c"
    echo "  $0 -I ./include -D DEBUG main.c utils.c"
    echo ""
    echo "Environment:"
    echo "  WASI_SDK_PATH    Path to WASI SDK (auto-detected if not set)"
}

# Error handling
error() {
    echo -e "${RED}Error: $1${NC}" >&2
    exit 1
}

# Info message
info() {
    if [ $VERBOSE -eq 1 ]; then
        echo -e "${GREEN}$1${NC}"
    fi
}

# Warning message
warn() {
    echo -e "${YELLOW}Warning: $1${NC}" >&2
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -o)
                OUTPUT="$2"
                shift 2
                ;;
            -O0|-O1|-O2|-O3|-Os|-Oz)
                OPT_LEVEL="$1"
                shift
                ;;
            -I)
                INCLUDES="$INCLUDES -I$2"
                shift 2
                ;;
            -I*)
                INCLUDES="$INCLUDES $1"
                shift
                ;;
            -D)
                DEFINES="$DEFINES -D$2"
                shift 2
                ;;
            -D*)
                DEFINES="$DEFINES $1"
                shift
                ;;
            -v|--verbose)
                VERBOSE=1
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            -*)
                error "Unknown option: $1"
                ;;
            *)
                SOURCES+=("$1")
                shift
                ;;
        esac
    done
    
    if [ ${#SOURCES[@]} -eq 0 ]; then
        error "No source files specified"
    fi
}

# Main build function
build() {
    # Find WASI SDK
    WASI_SDK=$(find_wasi_sdk)
    if [ -z "$WASI_SDK" ]; then
        error "WASI SDK not found. Please install it or set WASI_SDK_PATH"
    fi
    
    info "Using WASI SDK: $WASI_SDK"
    
    CC="$WASI_SDK/bin/clang"
    
    # Verify compiler exists
    if [ ! -x "$CC" ]; then
        error "Compiler not found: $CC"
    fi
    
    # Get script directory for default includes
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    AKIRA_ROOT="$(dirname "$SCRIPT_DIR")"
    
    # Add default include path for akira_api.h
    if [ -d "$AKIRA_ROOT/samples/wasm_apps/include" ]; then
        INCLUDES="-I$AKIRA_ROOT/samples/wasm_apps/include $INCLUDES"
    fi
    
    # Compiler flags
    CFLAGS="$OPT_LEVEL -nostdlib $INCLUDES $DEFINES"
    CFLAGS="$CFLAGS -Wall -Wextra -Wno-unused-parameter"
    
    # Linker flags
    LDFLAGS="-Wl,--no-entry"
    LDFLAGS="$LDFLAGS -Wl,--export=_start"
    LDFLAGS="$LDFLAGS -Wl,--allow-undefined"
    LDFLAGS="$LDFLAGS -Wl,--strip-all"
    
    # Build command
    CMD="$CC $CFLAGS $LDFLAGS -o $OUTPUT ${SOURCES[*]}"
    
    info "Building: ${SOURCES[*]}"
    info "Output: $OUTPUT"
    info "Optimization: $OPT_LEVEL"
    
    if [ $VERBOSE -eq 1 ]; then
        echo "Command: $CMD"
    fi
    
    # Execute build
    if $CMD; then
        # Success - show result
        SIZE=$(ls -la "$OUTPUT" | awk '{print $5}')
        echo -e "${GREEN}✓ Built successfully: $OUTPUT ($SIZE bytes)${NC}"
        
        # Show module info if verbose
        if [ $VERBOSE -eq 1 ] && command -v wasm-objdump &> /dev/null; then
            echo ""
            echo "=== Module Info ==="
            wasm-objdump -x "$OUTPUT" 2>/dev/null | head -40
        fi
        
        return 0
    else
        error "Build failed"
    fi
}

# Entry point
main() {
    parse_args "$@"
    build
}

main "$@"
