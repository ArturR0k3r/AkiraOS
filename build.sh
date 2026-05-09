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
#   Automatically discovered from boards/*.conf metadata.
#   Each .conf file declares: BOARD_ZEPHYR, BOARD_CHIP, BOARD_DESC (and optionally BOARD_ALIAS).
#   Run ./build.sh -h to see all available boards.
#
# Examples:
#   ./build.sh                              # Build and run native_sim
#   ./build.sh -b akiraconsole -bl y -r all # Build MCUboot + AkiraOS and flash
#   ./build.sh -b akiraconsole -r a         # Flash application only
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
ENABLE_PLATFORM=false
PORT=""
BAUD="921600"

# Version (read from VERSION file)
_ver_file="$SCRIPT_DIR/VERSION"
AKIRA_VERSION="$(
    major=$(grep 'VERSION_MAJOR' "$_ver_file" | sed 's/.*= *//')
    minor=$(grep 'VERSION_MINOR' "$_ver_file" | sed 's/.*= *//')
    patch=$(grep 'PATCHLEVEL'    "$_ver_file" | sed 's/.*= *//')
    echo "${major}.${minor}.${patch}"
)"

# Colors
RED=$'\033[0;31m'
GREEN=$'\033[0;32m'
YELLOW=$'\033[1;33m'
BLUE=$'\033[0;34m'
CYAN=$'\033[0;36m'
BOLD=$'\033[1m'
NC=$'\033[0m'

# =============================================================================
# Board Discovery (auto-loaded from boards/*.conf metadata)
# =============================================================================
declare -A BOARD_MAP=()
declare -A BOARD_CHIP=()
declare -A BOARD_DESC=()
declare -A BOARD_ALIASES=()  # primary_id -> space-separated short aliases
declare -a BOARD_PRIMARY=()  # ordered list of primary board IDs for display

load_boards() {
    local boards_dir="$SCRIPT_DIR/boards"
    local conf board_id zephyr chip desc aliases
    for conf in "$boards_dir"/*.conf; do
        [[ -f "$conf" ]] || continue
        board_id="$(basename "$conf" .conf)"
        zephyr="$(grep -m1 '^# BOARD_ZEPHYR:' "$conf" | sed 's/^# BOARD_ZEPHYR:[[:space:]]*//')"
        chip="$(grep -m1   '^# BOARD_CHIP:'   "$conf" | sed 's/^# BOARD_CHIP:[[:space:]]*//')"
        desc="$(grep -m1   '^# BOARD_DESC:'   "$conf" | sed 's/^# BOARD_DESC:[[:space:]]*//')"
        aliases="$(grep -m1 '^# BOARD_ALIAS:' "$conf" | sed 's/^# BOARD_ALIAS:[[:space:]]*//')"
        [[ -z "$zephyr" || -z "$chip" ]] && continue
        BOARD_MAP["$board_id"]="$zephyr"
        BOARD_CHIP["$board_id"]="$chip"
        BOARD_DESC["$board_id"]="${desc:-$board_id}"
        BOARD_PRIMARY+=("$board_id")
        if [[ -n "$aliases" ]]; then
            BOARD_ALIASES["$board_id"]="$aliases"
            for alias in $aliases; do
                BOARD_MAP["$alias"]="$zephyr"
                BOARD_CHIP["$alias"]="$chip"
                BOARD_DESC["$alias"]="${desc:-$board_id}"
            done
        fi
    done
}

load_boards

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
    cat << EOF
     _    _    _           ___  ____  
    / \  | | _(_)_ __ __ _/ _ \/ ___| 
   / _ \ | |/ / | '__/ _\` | | | \___ \ 
  / ___ \|   <| | | | (_| | |_| |___) |
 /_/   \_\_|\_\_|_|  \__,_|\___/|____/ 
                                       
         Unified Build System v${AKIRA_VERSION}
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
    --platform      Enable AkiraPlatform enterprise features
    -p <port>       Serial port for flashing (default: auto-detect)
    --baud <rate>   Baud rate for flashing (default: 921600)
    -h, --help      Show this help message

EOF
    echo -e "${BOLD}BOARDS:${NC}"
    for board in "${BOARD_PRIMARY[@]}"; do
        printf "    %-42s %s\n" "$board" "${BOARD_DESC[$board]}"
        if [[ -n "${BOARD_ALIASES[$board]:-}" ]]; then
            printf "    %-42s aliases: %s\n" "" "${BOARD_ALIASES[$board]}"
        fi
    done
    cat << EOF

${BOLD}EXAMPLES:${NC}
    ./build.sh
        Build and run native_sim (default)

    ./build.sh -b akiraconsole
        Build AkiraOS for Akira Console (ESP32-S3)

    ./build.sh -b akiraconsole -bl y
        Build MCUboot + AkiraOS for Akira Console

    ./build.sh -b akiraconsole -bl y -r all
        Build MCUboot + AkiraOS and flash both

    ./build.sh -b akiraconsole -r a
        Flash application only (no build)

    ./build.sh -b akiraconsole -e -r all
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
    local board_short="${BOARD//_/-}"
    echo "$WORKSPACE_ROOT/build-mcuboot-$board_short"
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

    # Always provide AkiraOS board root and DTS bindings so MCUboot sees the
    # same board overlay (and custom bindings) as the application build.
    # EXTRA_DTC_OVERLAY_FILE mirrors what sysbuild does automatically — it
    # ensures MCUboot's compiled-in partition layout matches the app's layout.
    local extra_cmake="-DBOARD_ROOT=$SCRIPT_DIR -DDTS_ROOT=$SCRIPT_DIR"
    # Prefer a board-specific MCUboot overlay (${BOARD}.mcuboot.overlay) when
    # present; otherwise fall back to the application overlay (${BOARD}.overlay).
    local mcuboot_overlay="$SCRIPT_DIR/boards/${BOARD}.mcuboot.overlay"
    local board_overlay="$SCRIPT_DIR/boards/${BOARD}.overlay"
    if [[ -f "$mcuboot_overlay" ]]; then
        extra_cmake+=" -DEXTRA_DTC_OVERLAY_FILE=$mcuboot_overlay"
        print_info "MCUboot overlay: $mcuboot_overlay"
    elif [[ -f "$board_overlay" ]]; then
        extra_cmake+=" -DEXTRA_DTC_OVERLAY_FILE=$board_overlay"
        print_info "MCUboot overlay: $board_overlay"
    fi

    # Pass board-specific MCUboot Kconfig overrides when present.
    # These live in AkiraOS/boards/<board>.mcuboot.conf so the mcuboot repo
    # stays at its upstream manifest-rev without local commits.
    local mcuboot_conf="$SCRIPT_DIR/boards/${BOARD}.mcuboot.conf"
    if [[ -f "$mcuboot_conf" ]]; then
        extra_cmake+=" -DEXTRA_CONF_FILE=$mcuboot_conf"
        print_info "MCUboot conf: $mcuboot_conf"
    fi

    # Some ESP32 SoCs require a patched MCUboot linker script to avoid IRAM
    # overflow (ESP32-C6) or RISC-V RVC relocation errors (ESP32-H2).  Zephyr's
    # SOC_LINKER_SCRIPT cmake variable is forced via CACHE INTERNAL and cannot
    # be overridden from the command line, so we temporarily replace the file in
    # the Zephyr source tree and restore it via git after the build.
    local _linker_patched_soc=""
    _apply_mcuboot_linker_patch() {
        local soc="$1"
        local akira_ld="$SCRIPT_DIR/zephyr/soc/espressif/${soc}/mcuboot.ld"
        local zephyr_ld="$WORKSPACE_ROOT/zephyr/soc/espressif/${soc}/mcuboot.ld"
        if [[ -f "$akira_ld" && -f "$zephyr_ld" ]]; then
            cp "$akira_ld" "$zephyr_ld"
            _linker_patched_soc="$soc"
            print_info "Applied MCUboot linker patch for $soc"
        fi
    }
    _restore_mcuboot_linker_patch() {
        if [[ -n "$_linker_patched_soc" ]]; then
            git -C "$WORKSPACE_ROOT/zephyr" checkout -- \
                "soc/espressif/${_linker_patched_soc}/mcuboot.ld" 2>/dev/null || true
            print_info "Restored MCUboot linker for ${_linker_patched_soc}"
        fi
    }

    case "$zephyr_board" in
        *esp32c6*) _apply_mcuboot_linker_patch "esp32c6" ;;
        *esp32h2*) _apply_mcuboot_linker_patch "esp32h2" ;;
    esac

    cd "$WORKSPACE_ROOT"

    local build_rc=0
    west build --pristine -b "$zephyr_board" bootloader/mcuboot/boot/zephyr -d "$build_dir" \
        -- $extra_cmake || build_rc=$?

    _restore_mcuboot_linker_patch

    if [[ $build_rc -eq 0 ]]; then
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
    
    # Build with optional AkiraPlatform modules
    local extra_cmake="-DMODULE_EXT_ROOT=$WORKSPACE_ROOT/AkiraOS"
    if [[ "$ENABLE_PLATFORM" == true ]]; then
        # Look for akira-platform in workspace (post-west-update) or parent dir
        local platform_path=""
        if [[ -d "$WORKSPACE_ROOT/akira-platform" ]]; then
            platform_path="$WORKSPACE_ROOT/akira-platform"
        elif [[ -d "$WORKSPACE_ROOT/../AkiraPlatform" ]]; then
            platform_path="$WORKSPACE_ROOT/../AkiraPlatform"
        else
            print_error "AkiraPlatform not found!"
            print_info "Either run: west update"
            print_info "Or ensure akira-platform exists in parent directory"
            exit 1
        fi
        extra_cmake+=" -DEXTRA_ZEPHYR_MODULES=$platform_path"
        print_info "AkiraPlatform enabled: $platform_path"
    fi
    
    if west build --pristine -b "$zephyr_board" AkiraOS -d "$build_dir" -- $extra_cmake; then
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
    
    # Flash bootloader and/or application.
    # Both are written in a SINGLE esptool invocation when flashing "all" to
    # avoid the chip booting into MCUboot between two separate connections
    # (which causes "Wrong boot mode detected" on the second connect).
    if [[ "$FLASH_TARGET" == "all" ]]; then
        local bootloader_bin="$mcuboot_dir/zephyr/zephyr.bin"
        local app_bin="$build_dir/zephyr/zephyr.signed.bin"
        if [[ ! -f "$app_bin" ]]; then
            app_bin="$build_dir/zephyr/zephyr.bin"
        fi

        if [[ ! -f "$bootloader_bin" ]]; then
            print_error "MCUboot binary not found: $bootloader_bin"
            print_info "Build bootloader first: ./build.sh -b $BOARD -bl o"
            exit 1
        fi
        if [[ ! -f "$app_bin" ]]; then
            print_error "Application binary not found in $build_dir/zephyr/"
            print_info "Build application first: ./build.sh -b $BOARD"
            exit 1
        fi

        local bootloader_offset="0x0"
        if [[ "$chip" == "esp32" ]]; then
            bootloader_offset="0x1000"
        fi

        print_step "Flashing MCUboot ($bootloader_offset) + AkiraOS (0x20000) in one pass"
        esptool --chip "$chip" --port "$PORT" --baud "$BAUD" \
            write_flash "$bootloader_offset" "$bootloader_bin" \
                        0x20000              "$app_bin"
        print_success "MCUboot + AkiraOS flashed!"
        return
    fi

    # Flash bootloader only
    if [[ "$FLASH_TARGET" == "b" ]]; then
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
    
    # Flash application only
    if [[ "$FLASH_TARGET" == "a" ]]; then
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
    local mcuboot_dir=$(get_mcuboot_build_dir)

    # Resolve app hex/bin (prefer signed image produced by Zephyr MCUboot integration)
    local app_hex="$build_dir/zephyr/zephyr.signed.hex"
    [[ -f "$app_hex" ]] || app_hex="$build_dir/zephyr/zephyr.hex"
    local mcuboot_hex="$mcuboot_dir/zephyr/zephyr.hex"

    # STM32CubeProgrammer (default runner for this board) passes --erase to the CLI,
    # which does a FULL chip erase.  Calling west flash twice therefore erases MCUboot
    # when the app is subsequently flashed.  Merge both hex files into a single file
    # so a single erase + write covers both regions.
    stm32_merge_hex() {
        local hex1="$1" hex2="$2" out="$3"
        python3 - "$hex1" "$hex2" "$out" << 'PYEOF'
import sys

def checksum(byte_vals):
    return (256 - (sum(byte_vals) & 0xFF)) & 0xFF

def parse_hex(filename):
    """Parse Intel HEX file into a list of (abs_address, bytes) chunks."""
    chunks = []
    ela = 0
    with open(filename) as f:
        for line in f:
            line = line.strip()
            if not line or line[0] != ':':
                continue
            byte_count = int(line[1:3], 16)
            addr16     = int(line[3:7], 16)
            rec_type   = int(line[7:9], 16)
            data       = bytes.fromhex(line[9:9 + byte_count * 2])
            if rec_type == 0:   # Data
                chunks.append((ela | addr16, data))
            elif rec_type == 1:  # EOF
                break
            elif rec_type == 2:  # Extended Segment Address
                ela = ((data[0] << 8) | data[1]) << 4
            elif rec_type == 4:  # Extended Linear Address
                ela = ((data[0] << 8) | data[1]) << 16
    return chunks

def write_hex(chunks, filename):
    """Write chunks sorted by address as a clean Intel HEX file."""
    chunks.sort(key=lambda c: c[0])
    current_ela = None
    lines = []
    for abs_addr, data in chunks:
        offset = 0
        while offset < len(data):
            row_addr = abs_addr + offset
            ela      = row_addr >> 16
            low      = row_addr & 0xFFFF
            if ela != current_ela:
                ela_hi, ela_lo = (ela >> 8) & 0xFF, ela & 0xFF
                cs = checksum([0x02, 0x00, 0x00, 0x04, ela_hi, ela_lo])
                lines.append(f':02000004{ela_hi:02X}{ela_lo:02X}{cs:02X}')
                current_ela = ela
            row     = data[offset:offset + 16]
            n       = len(row)
            addr_hi = (low >> 8) & 0xFF
            addr_lo = low & 0xFF
            cs = checksum([n, addr_hi, addr_lo, 0x00] + list(row))
            lines.append(f':{n:02X}{addr_hi:02X}{addr_lo:02X}00'
                         + ''.join(f'{b:02X}' for b in row)
                         + f'{cs:02X}')
            offset += 16
    lines.append(':00000001FF')
    with open(filename, 'w') as f:
        f.write('\n'.join(lines) + '\n')

chunks = parse_hex(sys.argv[1]) + parse_hex(sys.argv[2])
write_hex(chunks, sys.argv[3])
PYEOF
    }

    if [[ "$FLASH_TARGET" == "all" ]]; then
        # Single-pass: erase once, program MCUboot + app together
        if [[ ! -f "$mcuboot_hex" ]]; then
            print_error "MCUboot hex not found: $mcuboot_hex"
            print_info "Build bootloader first: ./build.sh -b $BOARD -bl o"
            exit 1
        fi
        if [[ ! -f "$app_hex" ]]; then
            print_error "App hex not found in $build_dir/zephyr/"
            exit 1
        fi
        local merged_hex
        merged_hex="$(mktemp /tmp/akira_merged_XXXXXX.hex)"
        print_info "Merging MCUboot + AkiraOS hex files..."
        stm32_merge_hex "$mcuboot_hex" "$app_hex" "$merged_hex"
        print_step "Flashing MCUboot + AkiraOS (STM32, single-pass)..."
        west flash -d "$mcuboot_dir" --hex-file "$merged_hex"
        rm -f "$merged_hex"
        print_success "MCUboot + AkiraOS flashed!"

    elif [[ "$FLASH_TARGET" == "b" ]]; then
        print_step "Flashing MCUboot (STM32)..."
        west flash -d "$mcuboot_dir"
        print_success "MCUboot flashed!"

    elif [[ "$FLASH_TARGET" == "a" ]]; then
        # App-only flash: merge with existing MCUboot hex to avoid erasing it
        if [[ -f "$mcuboot_hex" ]]; then
            local merged_hex
            merged_hex="$(mktemp /tmp/akira_merged_XXXXXX.hex)"
            print_info "Preserving MCUboot — merging before flash..."
            stm32_merge_hex "$mcuboot_hex" "$app_hex" "$merged_hex"
            west flash -d "$mcuboot_dir" --hex-file "$merged_hex"
            rm -f "$merged_hex"
        else
            print_warning "MCUboot not built — flashing app alone (MCUboot will be erased!)"
            west flash -d "$build_dir"
        fi
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
<<<<<<< HEAD
    "version": "1.5.x",
=======
    "version": "${AKIRA_VERSION}",
>>>>>>> 36047f5 (feat(boards): add nucleo_l476rg board support)
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
            --platform)
                ENABLE_PLATFORM=true
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
    echo "  Platform:    $ENABLE_PLATFORM"
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
