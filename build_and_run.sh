#!/bin/bash
# =============================================================================
# AkiraOS Native Simulator - Quick Build and Run Script
# =============================================================================
# This script builds and runs the AkiraOS native simulator with SDL2 support
#
# Usage:
#   ./build_and_run.sh           # Build and run
#   ./build_and_run.sh --clean   # Clean, build, and run
#   ./build_and_run.sh --run     # Run only (no build)
# =============================================================================

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

print_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_error()   { echo -e "${RED}[ERROR]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_step()    { echo -e "${CYAN}[STEP]${NC} ${BOLD}$1${NC}"; }

# =============================================================================
# Configuration
# =============================================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$WORKSPACE_ROOT/build-native-sim"
CLEAN=false
RUN_ONLY=false

# =============================================================================
# Parse Arguments
# =============================================================================
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--clean)
            CLEAN=true
            shift
            ;;
        -r|--run)
            RUN_ONLY=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  -c, --clean    Clean build artifacts before building"
            echo "  -r, --run      Run only (skip build)"
            echo "  -h, --help     Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0              # Build and run"
            echo "  $0 --clean      # Clean, build, and run"
            echo "  $0 --run        # Run only"
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# =============================================================================
# Requirements Check
# =============================================================================
check_requirements() {
    print_step "Checking requirements..."

    # Check west
    if ! command -v west &> /dev/null; then
        print_error "west not found! Please install Zephyr SDK"
        exit 1
    fi
    print_info "✓ west found"

    # Check SDL2
    if ! pkg-config --exists sdl2 2>/dev/null; then
        print_error "SDL2 development libraries not found!"
        echo ""
        echo "Please install SDL2:"
        echo "  Ubuntu/Debian: sudo apt-get install libsdl2-dev"
        echo "  Fedora:        sudo dnf install SDL2-devel"
        echo "  macOS:         brew install sdl2"
        exit 1
    fi
    print_info "✓ SDL2 found ($(pkg-config --modversion sdl2))"

    print_success "All requirements satisfied"
}

# =============================================================================
# Build Function
# =============================================================================
build_simulator() {
    print_step "Building AkiraOS Native Simulator..."

    if [[ "$CLEAN" == true ]]; then
        print_info "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
    fi

    cd "$WORKSPACE_ROOT"
    unset ZEPHYR_BASE

    print_info "Board: native_sim"
    print_info "Build dir: $BUILD_DIR"
    print_info "SDL2 Simulator: ENABLED"

    if west build --pristine -b native_sim AkiraOS -d "$BUILD_DIR" -- -DMODULE_EXT_ROOT="$WORKSPACE_ROOT/AkiraOS"; then
        print_success "Build complete!"
        echo ""
        print_info "Executable: $BUILD_DIR/zephyr/zephyr.exe"
    else
        print_error "Build failed!"
        exit 1
    fi
}

# =============================================================================
# Run Function
# =============================================================================
run_simulator() {
    local exe="$BUILD_DIR/zephyr/zephyr.exe"

    if [[ ! -f "$exe" ]]; then
        print_error "Executable not found: $exe"
        print_info "Build first with: $0"
        exit 1
    fi

    print_step "Starting AkiraOS Native Simulator with SDL2..."
    echo ""
    print_info "🎮 Keyboard Controls:"
    print_info "   WASD - D-Pad"
    print_info "   IJKL - Action buttons (X/B/Y/A)"
    print_info "   ESC  - Power button"
    print_info "   ENTER - Settings"
    echo ""
    print_info "Starting..."
    echo ""
    echo "════════════════════════════════════════════════════════════════"

    # Run the simulator
    "$exe"
}

# =============================================================================
# Main Execution
# =============================================================================
main() {
    echo ""
    echo "════════════════════════════════════════════════════════════════"
    echo "  AkiraOS Native Simulator with SDL2 Visual Display"
    echo "════════════════════════════════════════════════════════════════"
    echo ""

    check_requirements

    if [[ "$RUN_ONLY" == false ]]; then
        build_simulator
    fi

    echo ""
    run_simulator
}

# Run main function
main
