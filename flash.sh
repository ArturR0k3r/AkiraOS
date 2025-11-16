#!/bin/bash

# flash.sh - Flash MCUboot and AkiraOS to ESP32 variants
# Usage: ./flash.sh [OPTIONS]
# Options:
#   --platform PLATFORM  Specify platform: esp32, esp32s3, esp32c3 (default: auto-detect)
#   --bootloader-only    Flash only MCUboot
#   --app-only           Flash only AkiraOS application  
#   --port PORT          Specify serial port (default: auto-detect)
#   --baud BAUD          Specify baud rate (default: 921600)
#   --help               Show this help message

set -e  # Exit on any error

# Default values
FLASH_BOOTLOADER=true
FLASH_APP=true
PORT=""
BAUD="921600"
PLATFORM=""
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(dirname "$SCRIPT_DIR")"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error()   { echo -e "${RED}[ERROR]${NC} $1"; }

show_help() {
    cat << EOF
Usage: $0 [OPTIONS]

Flash MCUboot bootloader and AkiraOS application to ESP32 variants

PLATFORMS:
    esp32    - ESP32 DevKitC (Akira Console - Legacy)
    esp32s3  - ESP32-S3 DevKitM (Akira Console - Primary)
    esp32c3  - ESP32-C3 DevKitM (Akira Modules Only)

OPTIONS:
    --platform PLATFORM  Specify platform (default: auto-detect from chip)
    --bootloader-only    Flash only MCUboot bootloader
    --app-only           Flash only AkiraOS application
    --port PORT          Specify serial port (e.g., /dev/ttyUSB0, COM3)
    --baud BAUD          Specify baud rate (default: 921600)
    --help               Show this help message

FLASH ADDRESSES:
    MCUboot:  0x1000
    AkiraOS:  0x20000

EXAMPLES:
    $0                              # Auto-detect and flash both
    $0 --platform esp32s3           # Flash ESP32-S3 
    $0 --app-only --port /dev/ttyUSB0  # Flash app only to specific port

NOTE: ESP32-C3 is for Akira Modules only, not Akira Console!

EOF
}

# Parse args
while [[ $# -gt 0 ]]; do
    case $1 in
        --platform)        PLATFORM="$2"; shift 2 ;;
        --bootloader-only) FLASH_BOOTLOADER=true; FLASH_APP=false; shift ;;
        --app-only)        FLASH_BOOTLOADER=false; FLASH_APP=true; shift ;;
        --port)            PORT="$2"; shift 2 ;;
        --baud)            BAUD="$2"; shift 2 ;;
        --help)            show_help; exit 0 ;;
        *) print_error "Unknown option: $1"; show_help; exit 1 ;;
    esac
done

# Check esptool
check_esptool() {
    if ! command -v esptool &> /dev/null; then
        print_error "esptool not found! Please install it:"
        echo "  pip install esptool"
        exit 1
    fi
    ESPTOOL="esptool"
    print_info "Using esptool: $ESPTOOL"
}


# Detect port
detect_port() {
    if [[ -z "$PORT" ]]; then
        print_info "Auto-detecting ESP32 port..."
        for port in /dev/ttyUSB* /dev/ttyACM* /dev/cu.usbserial* /dev/cu.SLAB_USBtoUART*; do
            if [[ -e "$port" ]]; then
                PORT="$port"
                print_info "Found port: $PORT"
                break
            fi
        done
        if [[ -z "$PORT" ]]; then
            print_warning "Could not auto-detect port. Use --port"
            exit 1
        fi
    fi
}

# Detect chip type
detect_chip() {
    if [[ -z "$PLATFORM" ]]; then
        print_info "Auto-detecting chip type..."
        local chip_info=$($ESPTOOL --port "$PORT" --baud "$BAUD" chip_id 2>&1 || true)
        
        if echo "$chip_info" | grep -q "ESP32-S3"; then
            PLATFORM="esp32s3"
            print_info "Detected: ESP32-S3 (Akira Console)"
        elif echo "$chip_info" | grep -q "ESP32-C3"; then
            PLATFORM="esp32c3"
            print_warning "Detected: ESP32-C3 (Akira Modules Only - Not for Console!)"
        elif echo "$chip_info" | grep -q "ESP32"; then
            PLATFORM="esp32"
            print_info "Detected: ESP32 (Akira Console - Legacy)"
        else
            print_error "Could not detect chip type. Use --platform"
            exit 1
        fi
    else
        # Validate platform
        case "$PLATFORM" in
            esp32|esp32s3|esp32c3)
                print_info "Using specified platform: $PLATFORM"
                if [[ "$PLATFORM" == "esp32c3" ]]; then
                    print_warning "ESP32-C3 is for Akira Modules only, not Akira Console!"
                fi
                ;;
            *)
                print_error "Invalid platform: $PLATFORM"
                print_error "Valid platforms: esp32, esp32s3, esp32c3"
                exit 1
                ;;
        esac
    fi
}

# Set chip parameter for esptool
get_chip_param() {
    case "$PLATFORM" in
        esp32)   echo "esp32" ;;
        esp32s3) echo "esp32s3" ;;
        esp32c3) echo "esp32c3" ;;
        *)       echo "esp32" ;;
    esac
}

# Check binaries
check_binaries() {
    local missing_files=()
    if [[ "$FLASH_BOOTLOADER" == true ]]; then
        if [[ ! -f "$WORKSPACE_ROOT/build-mcuboot/zephyr/zephyr.bin" ]]; then
            missing_files+=("MCUboot: $WORKSPACE_ROOT/build-mcuboot/zephyr/zephyr.bin")
        fi
    fi
    if [[ "$FLASH_APP" == true ]]; then
        if [[ ! -f "$WORKSPACE_ROOT/build/zephyr/zephyr.signed.bin" ]]; then
            missing_files+=("AkiraOS: $WORKSPACE_ROOT/build/zephyr/zephyr.signed.bin")
        fi
    fi
    if [[ ${#missing_files[@]} -gt 0 ]]; then
        print_error "Missing binaries:"
        for f in "${missing_files[@]}"; do echo "  - $f"; done
        exit 1
    fi
}

flash_bootloader() {
    local bin="$WORKSPACE_ROOT/build-mcuboot/zephyr/zephyr.bin"
    local chip=$(get_chip_param)
    print_info "Flashing MCUboot -> 0x1000 (chip: $chip)"
    $ESPTOOL --chip "$chip" --port "$PORT" --baud "$BAUD" write-flash 0x1000 "$bin"
    print_success "MCUboot flashed!"
}

flash_application() {
    local bin="$WORKSPACE_ROOT/build/zephyr/zephyr.signed.bin"
    local chip=$(get_chip_param)
    print_info "Flashing AkiraOS -> 0x20000 (chip: $chip)"
    $ESPTOOL --chip "$chip" --port "$PORT" --baud "$BAUD" write-flash 0x20000 "$bin"
    print_success "AkiraOS flashed!"
}

show_device_info() {
    local chip=$(get_chip_param)
    print_info "Getting device info..."
    $ESPTOOL --chip "$chip" --port "$PORT" --baud "$BAUD" chip_id || true
    echo ""
}

main() {
    print_info "ESP32 Flash Script (MCUboot + AkiraOS)"
    echo "======================================"

    check_esptool
    detect_port
    detect_chip
    check_binaries

    echo ""
    print_info "Flash Config:"
    echo "  Platform: $PLATFORM"
    echo "  Port: $PORT"
    echo "  Baud: $BAUD"
    echo "  Bootloader: $FLASH_BOOTLOADER"
    echo "  Application: $FLASH_APP"
    echo ""

    show_device_info

    if [[ "$FLASH_BOOTLOADER" == true ]]; then
        flash_bootloader
        echo ""
    fi
    if [[ "$FLASH_APP" == true ]]; then
        flash_application
        echo ""
    fi

    print_success "Flashing done!"
    echo ""
    print_info "To monitor:"
    echo "  west espmonitor --port $PORT"
    echo "  or"
    echo "  screen $PORT 115200"
    echo "  or"
    echo "  picocom -b 115200 $PORT"
}

main "$@"
# End of flash.sh   