#!/bin/bash
#
# build_wasm_app.sh - Build WASM applications for AkiraOS
#
# This script provides a common build interface for all WASM apps targeting
# AkiraOS on WAMR with libc-builtin (NOT WASI) mode.
#
# Usage:
#   ./build_wasm_app.sh [options] <source_files...>
#
# Options:
#   -o <output>      Output filename (default: app.wasm)
#   -O0/-O1/-O2/-O3  Optimization level
#   -Os              Optimize for size (default)
#   -I <path>        Add include path
#   -D <define>      Add preprocessor definition
#   -m <stack_kb>    Set stack size in KB (default: 4)
#   -v               Verbose output
#   -h               Show help
#
# Examples:
#   ./build_wasm_app.sh main.c
#   ./build_wasm_app.sh -o my_app.wasm -O3 src/*.c
#   ./build_wasm_app.sh -I ./include -D DEBUG -m 8 main.c utils.c
#
# Environment:
#   WASI_SDK_PATH    Path to WASI SDK (auto-detected if not set)
#
# Note: Apps are built with -nostdlib for minimal size and to avoid WASI imports.
# Use external function declarations for env module functions (e.g., putchar).

set -e

# Default values
OUTPUT="app.wasm"
OPT_LEVEL="-Os"
INCLUDES=""
DEFINES=""
STACK_KB=4
VERBOSE=0
SOURCES=()

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
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
    cat << 'EOF'
AkiraOS WASM Application Builder
================================

Usage: build_wasm_app.sh [options] <source_files...>

Build WASM applications for AkiraOS running on WAMR with libc-builtin mode.

OPTIONS:
  -o <file>        Output filename (default: app.wasm)
  -O0/-O1/-O2/-O3  Optimization level (default: -Os)
  -Os              Optimize for size (default)
  -I <path>        Add include path (can be used multiple times)
  -D <define>      Add preprocessor definition (can be used multiple times)
  -m <stack_kb>    Stack size in KB (default: 4)
  -v, --verbose    Verbose output with build details
  -h, --help       Show this help message

EXAMPLES:
  # Simple single file
  build_wasm_app.sh main.c

  # Custom output with optimization
  build_wasm_app.sh -o my_app.wasm -O3 main.c

  # With includes and defines
  build_wasm_app.sh -I ./include -D DEBUG -D APP_VERSION=1 main.c utils.c

  # Custom stack size
  build_wasm_app.sh -m 8 main.c

ENVIRONMENT:
  WASI_SDK_PATH    Path to WASI SDK (auto-detected if not set)

BUILD DETAILS:
  - Apps compile with -nostdlib to avoid WASI imports
  - Uses env module for runtime functions (putchar, etc.)
  - Memory limited to 64KB for embedded targets
  - All symbols stripped for minimal size
EOF
}

# Error handling
error() {
    echo -e "${RED}✗ Error: $1${NC}" >&2
    exit 1
}

# Info message
info() {
    if [ $VERBOSE -eq 1 ]; then
        echo -e "${BLUE}ℹ $1${NC}"
    fi
}

# Success message
success() {
    echo -e "${GREEN}✓ $1${NC}"
}

# Warning message
warn() {
    echo -e "${YELLOW}⚠ Warning: $1${NC}" >&2
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
            -m)
                STACK_KB="$2"
                shift 2
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
        error "No source files specified. Use -h for help."
    fi
}

# Get file size in human readable format
human_size() {
    local bytes=$1
    if [ $bytes -lt 1024 ]; then
        echo "${bytes}B"
    elif [ $bytes -lt 1048576 ]; then
        echo "$(( bytes / 1024 ))KB"
    else
        echo "$(( bytes / 1048576 ))MB"
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
        info "Added AkiraOS include path"
    fi
    
    # Calculate memory (stack must fit in 64KB linear memory)
    STACK_BYTES=$((STACK_KB * 1024))
    
    # Compiler flags - critical for embedded WAMR
    # -nostdlib: Avoid WASI imports, only use what we provide
    # -O level: Optimization for code size
    CFLAGS="$OPT_LEVEL -nostdlib $INCLUDES $DEFINES"
    CFLAGS="$CFLAGS -Wall -Wextra -Wno-unused-parameter -Wno-unknown-attributes"
    
    # Linker flags for embedded targets
    # --no-entry: No default _start entry point
    # --export=main: Export main function for OCRE runtime
    # --allow-undefined: Allow imports from env module
    # --strip-all: Remove symbols to minimize size
    # Memory limits: 64KB fits in WAMR heap
    LDFLAGS="-Wl,--no-entry"
    LDFLAGS="$LDFLAGS -Wl,--export=main"
    LDFLAGS="$LDFLAGS -Wl,--allow-undefined"
    LDFLAGS="$LDFLAGS -Wl,--strip-all"
    LDFLAGS="$LDFLAGS -z stack-size=$STACK_BYTES"
    LDFLAGS="$LDFLAGS -Wl,--initial-memory=65536"
    LDFLAGS="$LDFLAGS -Wl,--max-memory=65536"
    
    # Build command
    CMD="$CC $CFLAGS $LDFLAGS -o $OUTPUT ${SOURCES[*]}"
    
    echo -e "${BLUE}═══════════════════════════════════════════${NC}"
    echo -e "${BLUE}Building WASM Application${NC}"
    echo -e "${BLUE}═══════════════════════════════════════════${NC}"
    
    info "Source files: ${SOURCES[*]}"
    info "Output: $OUTPUT"
    info "Optimization: $OPT_LEVEL"
    info "Stack size: ${STACK_KB}KB"
    
    if [ $VERBOSE -eq 1 ]; then
        echo ""
        echo "Compiler: $CC"
        echo "CFLAGS: $CFLAGS"
        echo "LDFLAGS: $LDFLAGS"
        echo "Command: $CMD"
        echo ""
    fi
    
    # Execute build
    if $CMD; then
        # Success - show result
        if [ -f "$OUTPUT" ]; then
            SIZE=$(ls -la "$OUTPUT" | awk '{print $5}')
            SIZE_HUMAN=$(human_size $SIZE)
            success "Built successfully: $OUTPUT ($SIZE_HUMAN, $SIZE bytes)"
            
            # Show memory info if verbose
            if [ $VERBOSE -eq 1 ] && command -v wasm-objdump &> /dev/null; then
                echo ""
                echo "=== WASM Module Info ==="
                wasm-objdump -x "$OUTPUT" 2>/dev/null | head -30 || true
            fi
        else
            error "Output file not created"
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

