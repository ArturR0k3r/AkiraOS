#!/bin/bash
# =============================================================================
# AkiraOS Unified Build Script
# =============================================================================
# Usage: ./build.sh [options]
#
# Options:
#   -b <board>      Select board (default: native_sim)
#   -bl y           Build bootloader with application
#   -bl o           Build bootloader only
#   -r a            Flash application after build
#   -r b            Flash bootloader after build
#   -r all          Flash both bootloader and application
#   -e              Erase flash before flashing
#   -s              Generate SBOM (Software Bill of Materials)
#   -c              Clean build artifacts
#   --full-clean    Reset to pristine state (remove all build dirs)
#   -h, --help      Show this help message
#
# Boards:
#   native_sim                         Native simulator (default)
#   esp32s3_devkitm_esp32s3_procpu     ESP32-S3 DevKitM (Akira Console)
#   esp32s3_supermini_esp32s3_procpu   ESP32-S3 Super Mini (compact)
#   esp32_devkitc_procpu               ESP32 DevKitC (Akira Micro)
#   nrf54l15dk_nrf54l15_cpuapp         nRF54L15 DK (Nordic)
#   steval_stwinbx1                    STM32 STWIN.box
#   b_u585i_iot02a                     STM32U5 IoT Discovery Kit
#
# Examples:
#   ./build.sh                              # Build and run native_sim
#   ./build.sh -b esp32s3_devkitm_esp32s3_procpu -bl y -r all
#   ./build.sh -b esp32s3_devkitm_esp32s3_procpu -r a
#   ./build.sh -c                           # Clean all builds
# =============================================================================

set -e

# =============================================================================
# Configuration
# =============================================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(dirname "$SCRIPT_DIR")"

# Defaults
BOARD="native_sim"
BUILD_BOOTLOADER=""       # "", "y" (with app), "o" (only)
FLASH_TARGET=""           # "", "a" (app), "b" (bootloader), "all"
ERASE_FLASH=false
GENERATE_SBOM=false
CLEAN_BUILD=false
FULL_CLEAN=false
PORT=""
BAUD="921600"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# =============================================================================
# Board Mappings
# =============================================================================
declare -A BOARD_MAP=(
    ["native_sim"]="native_sim"
    ["esp32s3_devkitm_esp32s3_procpu"]="esp32s3_devkitm/esp32s3/procpu"
    ["akiraconsole_esp32s3_procpu"]="akiraconsole/esp32s3/procpu"
    ["esp32s3_supermini_esp32s3_procpu"]="esp32s3_supermini/esp32s3/procpu"
    ["esp32c3_devkitm"]="esp32c3_devkitm"
    ["esp32_devkitc_procpu"]="esp32_devkitc/esp32/procpu"
    ["nrf54l15dk_nrf54l15_cpuapp"]="nrf54l15dk/nrf54l15/cpuapp"
    ["steval_stwinbx1"]="steval_stwinbx1"
    ["b_u585i_iot02a"]="b_u585i_iot02a"
)

declare -A BOARD_CHIP=(
    ["native_sim"]="native"
    ["esp32s3_devkitm_esp32s3_procpu"]="esp32s3"
    ["akiraconsole_esp32s3_procpu"]="esp32s3"
    ["esp32s3_supermini_esp32s3_procpu"]="esp32s3"
    ["esp32c3_devkitm"]="esp32c3"
    ["esp32_devkitc_procpu"]="esp32"
    ["nrf54l15dk_nrf54l15_cpuapp"]="nrf54l15"
    ["steval_stwinbx1"]="stm32"
    ["b_u585i_iot02a"]="stm32"
)

declare -A BOARD_DESC=(
    ["native_sim"]="Native Simulator"
    ["esp32s3_devkitm_esp32s3_procpu"]="ESP32-S3 DevKitM (Akira Console)"
    ["akiraconsole_esp32s3_procpu"]="Akira Console (ESP32-S3 DevKitM)"
    ["esp32s3_supermini_esp32s3_procpu"]="ESP32-S3 Super Mini"
    ["esp32c3_devkitm"]="ESP32-C3 DevKitM (RISC-V)"
    ["esp32_devkitc_procpu"]="ESP32 DevKitC (Akira Micro)"
    ["nrf54l15dk_nrf54l15_cpuapp"]="Nordic nRF54L15 DK"
    ["steval_stwinbx1"]="ST STEVAL-STWINBX1"
    ["b_u585i_iot02a"]="ST B-U585I-IOT02A Discovery Kit"
)

# =============================================================================
# Helper Functions
# =============================================================================
print_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error()   { echo -e "${RED}[ERROR]${NC} $1"; }
print_step()    { echo -e "${CYAN}${BOLD}==> $1${NC}"; }

show_banner() {
    echo -e "${CYAN}"
    cat << 'EOF'
     _    _    _           ___  ____  
    / \  | | _(_)_ __ __ _/ _ \/ ___| 
   / _ \ | |/ / | '__/ _` | | | \___ \ 
  / ___ \|   <| | | | (_| | |_| |___) |
 /_/   \_\_|\_\_|_|  \__,_|\___/|____/ 
                                       
         Unified Build System v1.4.x
EOF
    echo -e "${NC}"
}

show_help() {
    show_banner
    cat << EOF
${BOLD}USAGE:${NC}
    ./build.sh [options]

${BOLD}OPTIONS:${NC}
    -b <board>      Select target board (default: native_sim)
    -bl y           Build bootloader WITH application
    -bl o           Build bootloader ONLY
    -r a            Flash application after build
    -r b            Flash bootloader after build
    -r all          Flash both bootloader and application
    -e              Erase flash before flashing
    -s              Generate SBOM (Software Bill of Materials)
    -c              Clean build artifacts for selected board
    --full-clean    Reset to pristine state (remove ALL build dirs)
    -p <port>       Serial port for flashing (default: auto-detect)
    --baud <rate>   Baud rate for flashing (default: 921600)
    -h, --help      Show this help message

${BOLD}BOARDS:${NC}
    native_sim                         Native simulator (default)
    esp32s3_devkitm_esp32s3_procpu     ESP32-S3 DevKitM (Akira Console)
    akiraconsole_esp32s3_procpu        Akira Console (ESP32-S3 DevKitM)
    esp32s3_supermini_esp32s3_procpu   ESP32-S3 Super Mini (compact)
    esp32c3_devkitm                    ESP32-C3 DevKitM (RISC-V)
    esp32_devkitc_procpu               ESP32 DevKitC (Legacy)
    nrf54l15dk_nrf54l15_cpuapp         Nordic nRF54L15 DK
    steval_stwinbx1                    ST STEVAL-STWINBX1

${BOLD}EXAMPLES:${NC}
    ./build.sh
        Build and run native_sim (default)

    ./build.sh -b esp32s3_devkitm_esp32s3_procpu
        Build AkiraOS for ESP32-S3

    ./build.sh -b esp32s3_devkitm_esp32s3_procpu -bl y
        Build MCUboot + AkiraOS for ESP32-S3

    ./build.sh -b esp32s3_devkitm_esp32s3_procpu -bl y -r all
        Build MCUboot + AkiraOS and flash both

    ./build.sh -b esp32s3_devkitm_esp32s3_procpu -r a
        Flash application only (no build)

    ./build.sh -b esp32s3_devkitm_esp32s3_procpu -e -r all
        Erase flash, then flash bootloader + app

    ./build.sh -c
        Clean build artifacts

    ./build.sh --full-clean
        Remove all build directories

${BOLD}FLASH ADDRESSES (ESP32):${NC}
    MCUboot:     0x1000
    Application: 0x20000

EOF
}

# =============================================================================
# Validation Functions
# =============================================================================
validate_board() {
    if [[ -z "${BOARD_MAP[$BOARD]}" ]]; then
        print_error "Unknown board: $BOARD"
        echo ""
        echo "Available boards:"
        for b in "${!BOARD_MAP[@]}"; do
            echo "  - $b (${BOARD_DESC[$b]})"
        done
        exit 1
    fi
}

check_tools() {
    local chip="${BOARD_CHIP[$BOARD]}"
    
    # Check west
    if ! command -v west &> /dev/null; then
        print_error "west not found! Please install Zephyr SDK"
        exit 1
    fi
    
    # Check esptool for ESP32 boards
    if [[ "$chip" == "esp32"* ]] && [[ -n "$FLASH_TARGET" ]]; then
        if ! command -v esptool &> /dev/null; then
            print_error "esptool not found! Install with: pip install esptool"
            exit 1
        fi
    fi
}

# =============================================================================
# Build Functions
# =============================================================================
get_build_dir() {
    local board_short="${BOARD//_/-}"
    echo "$WORKSPACE_ROOT/build-$board_short"
}

get_mcuboot_build_dir() {
    echo "$WORKSPACE_ROOT/build-mcuboot"
}

clean_build() {
    print_step "Cleaning build artifacts..."
    
    if [[ "$FULL_CLEAN" == true ]]; then
        print_info "Full clean: Removing all build directories..."
        rm -rf "$WORKSPACE_ROOT"/build*
        print_success "All build directories removed"
    else
        local build_dir=$(get_build_dir)
        local mcuboot_dir=$(get_mcuboot_build_dir)
        
        if [[ -d "$build_dir" ]]; then
            rm -rf "$build_dir"
            print_info "Removed: $build_dir"
        fi
        
        if [[ -d "$mcuboot_dir" ]]; then
            rm -rf "$mcuboot_dir"
            print_info "Removed: $mcuboot_dir"
        fi
        
        print_success "Build artifacts cleaned for $BOARD"
    fi
}

build_mcuboot() {
    local zephyr_board="${BOARD_MAP[$BOARD]}"
    local build_dir=$(get_mcuboot_build_dir)
    
    print_step "Building MCUboot bootloader..."
    print_info "Board: $zephyr_board"
    print_info "Build dir: $build_dir"
    
    cd "$WORKSPACE_ROOT"
    
    # Include custom board root for boards defined in AkiraOS/boards
    if west build -b "$zephyr_board" bootloader/mcuboot/boot/zephyr -d "$build_dir" \
        -- -DBOARD_ROOT="$SCRIPT_DIR"; then
        print_success "MCUboot build complete!"
        print_info "Binary: $build_dir/zephyr/zephyr.bin"
    else
        print_error "MCUboot build failed!"
        exit 1
    fi
}

build_application() {
    local zephyr_board="${BOARD_MAP[$BOARD]}"
    local build_dir=$(get_build_dir)
    
    print_step "Building AkiraOS application..."
    print_info "Board: $zephyr_board"
    print_info "Build dir: $build_dir"
    
    cd "$WORKSPACE_ROOT"
    unset ZEPHYR_BASE
    
    if west build --pristine -b "$zephyr_board" AkiraOS -d "$build_dir" -- -DMODULE_EXT_ROOT="$WORKSPACE_ROOT/AkiraOS"; then
        print_success "AkiraOS build complete!"
        print_info "Binary: $build_dir/zephyr/zephyr.bin"
        
        # Show memory usage for non-native builds
        if [[ "$BOARD" != "native_sim" ]]; then
            echo ""
            print_info "Memory usage:"
            if [[ -f "$build_dir/zephyr/zephyr.map" ]]; then
                size "$build_dir/zephyr/zephyr.elf" 2>/dev/null || true
            fi
        fi
    else
        print_error "AkiraOS build failed!"
        exit 1
    fi
}

run_native_sim() {
    local build_dir=$(get_build_dir)
    local exe="$build_dir/zephyr/zephyr.exe"
    
    if [[ ! -f "$exe" ]]; then
        print_error "Executable not found: $exe"
        print_info "Run build first: ./build.sh"
        exit 1
    fi
    
    print_step "Running AkiraOS native simulator..."
    echo ""
    "$exe"
}

# =============================================================================
# Flash Functions
# =============================================================================
detect_port() {
    if [[ -n "$PORT" ]]; then
        return
    fi
    
    print_info "Auto-detecting serial port..."
    
    for port in /dev/ttyUSB* /dev/ttyACM* /dev/cu.usbserial* /dev/cu.SLAB_USBtoUART*; do
        if [[ -e "$port" ]]; then
            PORT="$port"
            print_info "Found port: $PORT"
            return
        fi
    done
    
    print_error "No serial port detected. Use -p <port> to specify."
    exit 1
}

get_esptool_chip() {
    local chip="${BOARD_CHIP[$BOARD]}"
    case "$chip" in
        esp32s3) echo "esp32s3" ;;
        esp32)   echo "esp32" ;;
        esp32c3) echo "esp32c3" ;;
        *)       echo "" ;;
    esac
}

erase_flash() {
    local chip=$(get_esptool_chip)
    
    if [[ -z "$chip" ]]; then
        print_warning "Erase not supported for this board"
        return
    fi
    
    print_step "Erasing flash..."
    esptool --chip "$chip" --port "$PORT" --baud "$BAUD" erase_flash
    print_success "Flash erased!"
}

flash_esp32() {
    local chip=$(get_esptool_chip)
    local build_dir=$(get_build_dir)
    local mcuboot_dir=$(get_mcuboot_build_dir)
    
    detect_port
    
    if [[ "$ERASE_FLASH" == true ]]; then
        erase_flash
    fi
    
    # Flash bootloader
    if [[ "$FLASH_TARGET" == "b" || "$FLASH_TARGET" == "all" ]]; then
        local bootloader_bin="$mcuboot_dir/zephyr/zephyr.bin"
        
        if [[ ! -f "$bootloader_bin" ]]; then
            print_error "MCUboot binary not found: $bootloader_bin"
            print_info "Build bootloader first: ./build.sh -b $BOARD -bl o"
            exit 1
        fi
        
        # ESP32 classic needs bootloader at 0x1000, others at 0x0
        local bootloader_offset="0x0"
        if [[ "$chip" == "esp32" ]]; then
            bootloader_offset="0x1000"
        fi
        
        print_step "Flashing MCUboot -> $bootloader_offset"
        esptool --chip "$chip" --port "$PORT" --baud "$BAUD" write_flash "$bootloader_offset" "$bootloader_bin"
        print_success "MCUboot flashed!"
    fi
    
    # Flash application
    if [[ "$FLASH_TARGET" == "a" || "$FLASH_TARGET" == "all" ]]; then
        # Try signed binary first, fall back to unsigned
        local app_bin="$build_dir/zephyr/zephyr.signed.bin"
        if [[ ! -f "$app_bin" ]]; then
            app_bin="$build_dir/zephyr/zephyr.bin"
        fi
        
        if [[ ! -f "$app_bin" ]]; then
            print_error "Application binary not found in $build_dir/zephyr/"
            print_info "Build application first: ./build.sh -b $BOARD"
            exit 1
        fi
        
        print_step "Flashing AkiraOS -> 0x20000"
        esptool --chip "$chip" --port "$PORT" --baud "$BAUD" write_flash 0x20000 "$app_bin"
        print_success "AkiraOS flashed!"
    fi
}

flash_nordic() {
    local build_dir=$(get_build_dir)
    
    # Flash bootloader
    if [[ "$FLASH_TARGET" == "b" || "$FLASH_TARGET" == "all" ]]; then
        local mcuboot_dir=$(get_mcuboot_build_dir)
        print_step "Flashing MCUboot (Nordic)..."
        west flash -d "$mcuboot_dir"
        print_success "MCUboot flashed!"
    fi
    
    # Flash application
    if [[ "$FLASH_TARGET" == "a" || "$FLASH_TARGET" == "all" ]]; then
        print_step "Flashing AkiraOS (Nordic)..."
        west flash -d "$build_dir"
        print_success "AkiraOS flashed!"
    fi
}

flash_stm32() {
    local build_dir=$(get_build_dir)
    
    # Flash bootloader
    if [[ "$FLASH_TARGET" == "b" || "$FLASH_TARGET" == "all" ]]; then
        local mcuboot_dir=$(get_mcuboot_build_dir)
        print_step "Flashing MCUboot (STM32)..."
        west flash -d "$mcuboot_dir"
        print_success "MCUboot flashed!"
    fi
    
    # Flash application
    if [[ "$FLASH_TARGET" == "a" || "$FLASH_TARGET" == "all" ]]; then
        print_step "Flashing AkiraOS (STM32)..."
        west flash -d "$build_dir"
        print_success "AkiraOS flashed!"
    fi
}

flash_board() {
    local chip="${BOARD_CHIP[$BOARD]}"
    
    print_step "Flashing to ${BOARD_DESC[$BOARD]}..."
    
    case "$chip" in
        esp32*)
            flash_esp32
            ;;
        nrf*)
            flash_nordic
            ;;
        stm32)
            flash_stm32
            ;;
        native)
            print_warning "Cannot flash native_sim - use run instead"
            ;;
        *)
            print_error "Flashing not supported for chip: $chip"
            exit 1
            ;;
    esac
}

# =============================================================================
# SBOM Generation
# =============================================================================
generate_sbom() {
    local build_dir=$(get_build_dir)
    local sbom_file="$build_dir/sbom.json"
    
    print_step "Generating Software Bill of Materials..."
    
    # Use west to generate SBOM if available
    if west blobs --help &> /dev/null; then
        # Basic SBOM from build info
        cat > "$sbom_file" << EOF
{
    "name": "AkiraOS",
    "version": "1.4.x",
    "board": "$BOARD",
    "zephyr_board": "${BOARD_MAP[$BOARD]}",
    "build_date": "$(date -Iseconds)",
    "components": [
        {"name": "Zephyr RTOS", "version": "4.3.0"},
        {"name": "WASM Micro Runtime", "version": "2.3.0"},
        {"name": "MCUboot", "version": "2.0.0"}
    ]
}
EOF
        print_success "SBOM generated: $sbom_file"
    else
        print_warning "SBOM generation requires west blobs support"
    fi
}

# =============================================================================
# Parse Arguments
# =============================================================================
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -b)
                BOARD="$2"
                shift 2
                ;;
            -bl)
                BUILD_BOOTLOADER="$2"
                if [[ "$BUILD_BOOTLOADER" != "y" && "$BUILD_BOOTLOADER" != "o" ]]; then
                    print_error "Invalid -bl option: $BUILD_BOOTLOADER (use 'y' or 'o')"
                    exit 1
                fi
                shift 2
                ;;
            -r)
                FLASH_TARGET="$2"
                if [[ "$FLASH_TARGET" != "a" && "$FLASH_TARGET" != "b" && "$FLASH_TARGET" != "all" ]]; then
                    print_error "Invalid -r option: $FLASH_TARGET (use 'a', 'b', or 'all')"
                    exit 1
                fi
                shift 2
                ;;
            -e)
                ERASE_FLASH=true
                shift
                ;;
            -s)
                GENERATE_SBOM=true
                shift
                ;;
            -c)
                CLEAN_BUILD=true
                shift
                ;;
            --full-clean)
                FULL_CLEAN=true
                CLEAN_BUILD=true
                shift
                ;;
            -p)
                PORT="$2"
                shift 2
                ;;
            --baud)
                BAUD="$2"
                shift 2
                ;;
            -h|--help)
                show_help
                exit 0
                ;;
            *)
                print_error "Unknown option: $1"
                echo "Use -h for help"
                exit 1
                ;;
        esac
    done
}

# =============================================================================
# Main
# =============================================================================
main() {
    parse_args "$@"
    
    show_banner
    
    # Validate board
    validate_board
    check_tools
    
    echo -e "${BOLD}Configuration:${NC}"
    echo "  Board:       $BOARD (${BOARD_DESC[$BOARD]})"
    echo "  Zephyr:      ${BOARD_MAP[$BOARD]}"
    echo "  Bootloader:  ${BUILD_BOOTLOADER:-no}"
    echo "  Flash:       ${FLASH_TARGET:-no}"
    echo "  Erase:       $ERASE_FLASH"
    echo "  SBOM:        $GENERATE_SBOM"
    echo "  Clean:       $CLEAN_BUILD"
    echo ""
    
    # Clean if requested
    if [[ "$CLEAN_BUILD" == true ]]; then
        clean_build
        if [[ "$FULL_CLEAN" == true ]]; then
            exit 0
        fi
    fi
    
    # Only flash (no build)
    if [[ -n "$FLASH_TARGET" && -z "$BUILD_BOOTLOADER" && "$CLEAN_BUILD" != true ]]; then
        # Check if we need to build first
        local build_dir=$(get_build_dir)
        if [[ ! -d "$build_dir" ]]; then
            print_info "No existing build found, building first..."
            build_application
        fi
        flash_board
        exit 0
    fi
    
    # Build bootloader only
    if [[ "$BUILD_BOOTLOADER" == "o" ]]; then
        build_mcuboot
        
        if [[ "$FLASH_TARGET" == "b" || "$FLASH_TARGET" == "all" ]]; then
            flash_board
        fi
        
        exit 0
    fi
    
    # Build bootloader with application
    if [[ "$BUILD_BOOTLOADER" == "y" ]]; then
        build_mcuboot
    fi
    
    # Build application (default action)
    if [[ "$CLEAN_BUILD" != true || -n "$BUILD_BOOTLOADER" ]]; then
        build_application
    fi
    
    # Generate SBOM if requested
    if [[ "$GENERATE_SBOM" == true ]]; then
        generate_sbom
    fi
    
    # Flash if requested
    if [[ -n "$FLASH_TARGET" ]]; then
        flash_board
    fi
    
    # Run native_sim automatically
    if [[ "$BOARD" == "native_sim" && -z "$FLASH_TARGET" && "$CLEAN_BUILD" != true ]]; then
        echo ""
        run_native_sim
    fi
    
    echo ""
    print_success "Done!"
    
    # Show next steps for non-native builds
    if [[ "$BOARD" != "native_sim" && -z "$FLASH_TARGET" ]]; then
        echo ""
        echo -e "${BOLD}Next steps:${NC}"
        echo "  Flash:   ./build.sh -b $BOARD -r all"
        echo "  Monitor: west espmonitor (ESP32) or screen /dev/ttyUSB0 115200"
    fi
}

main "$@"
