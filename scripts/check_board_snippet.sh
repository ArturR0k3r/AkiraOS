#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Checks that per-board AkiraOS configuration produces the same Kconfig and
# devicetree output after it moves between the reference app's boards/ folder
# and the akira-board snippet (or any later restructuring).
#
#   scripts/check_board_snippet.sh snapshot <dir> [board-id ...]
#   scripts/check_board_snippet.sh compare  <dir> [board-id ...]
#
# snapshot: configure every board (cmake only) and store .config and zephyr.dts.
# compare:  configure again and diff against the snapshot; also requires the
#           board's akira-board snippet conf to be merged.
# Board ids are the conf file names (e.g. native_sim, rpi_pico) that carry a
# "# BOARD_ZEPHYR:" header. Set JOBS to change parallelism (default 4).
# MCUboot devicetrees are checked for boards that have a bootloader partition
# layout (MCUBOOT_BOARDS below).

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:?usage: $0 snapshot|compare <dir> [board-id ...]}"
OUT="${2:?usage: $0 snapshot|compare <dir> [board-id ...]}"
shift 2
JOBS="${JOBS:-4}"
MCUBOOT_BOARDS="b_u585i_iot02a nucleo_h743zi nucleo_l476rg rpi_pico rpi_pico2_rp2350a_m33 esp32h2_devkitm_esp32h2 esp32c6_devkitc_esp32c6_hpcore"

conf_for() {
    local id="$1"
    for dir in "$REPO/snippets/akira-board/boards" "$REPO/boards"; do
        [[ -f "$dir/$id.conf" ]] && { echo "$dir/$id.conf"; return; }
    done
}

list_boards() {
    for dir in "$REPO/boards" "$REPO/snippets/akira-board/boards"; do
        [[ -d "$dir" ]] || continue
        grep -l '^# BOARD_ZEPHYR:' "$dir"/*.conf 2>/dev/null || true
    done | xargs -r -n1 basename | sed 's/\.conf$//' | sort -u
}

# zephyr.dts annotates nodes with the file that defined them ("defined in
# <path>"), which changes whenever a file moves; compare without comments.
strip_dts() { sed -E 's#/\*[^*]*(\*+[^*/][^*]*)*\*+/##g; s/[[:space:]]+$//' "$1"; }

configure_app() {
    local id="$1" conf zephyr_board work
    conf="$(conf_for "$id")"
    zephyr_board="$(grep -m1 '^# BOARD_ZEPHYR:' "$conf" | sed 's/^# BOARD_ZEPHYR:[[:space:]]*//')"
    work="$OUT/work/app-$id"
    mkdir -p "$OUT/$MODE/$id"
    if ! (cd "$REPO" && west build -b "$zephyr_board" . -d "$work" -p always --cmake-only) > "$OUT/$MODE/$id/app.log" 2>&1; then
        echo "FAIL configure app $id ($zephyr_board): see $OUT/$MODE/$id/app.log"
        return 0
    fi
    cp "$work/zephyr/.config" "$OUT/$MODE/$id/config"
    strip_dts "$work/zephyr/zephyr.dts" > "$OUT/$MODE/$id/zephyr.dts"
    echo "ok   app $id ($zephyr_board)"
}

configure_mcuboot() {
    local id="$1" conf zephyr_board work overlay mconf args
    conf="$(conf_for "$id")"
    zephyr_board="$(grep -m1 '^# BOARD_ZEPHYR:' "$conf" | sed 's/^# BOARD_ZEPHYR:[[:space:]]*//')"
    work="$OUT/work/mcuboot-$id"
    args=(-DBOARD_ROOT="$REPO" -DDTS_ROOT="$REPO")
    overlay=""
    for candidate in "$REPO/boards/$id.mcuboot.overlay" "$REPO/snippets/akira-board/boards/$id.overlay" "$REPO/boards/$id.overlay"; do
        [[ -f "$candidate" ]] && { overlay="$candidate"; break; }
    done
    [[ -n "$overlay" ]] && args+=(-DEXTRA_DTC_OVERLAY_FILE="$overlay")
    mconf="$REPO/boards/$id.mcuboot.conf"
    [[ -f "$mconf" ]] && args+=(-DEXTRA_CONF_FILE="$mconf")
    mkdir -p "$OUT/$MODE/$id"
    if ! (cd "$REPO/.." && west build -b "$zephyr_board" bootloader/mcuboot/boot/zephyr -d "$work" -p always --cmake-only -- "${args[@]}") > "$OUT/$MODE/$id/mcuboot.log" 2>&1; then
        echo "FAIL configure mcuboot $id: see $OUT/$MODE/$id/mcuboot.log"
        return 0
    fi
    strip_dts "$work/zephyr/zephyr.dts" > "$OUT/$MODE/$id/mcuboot.dts"
    echo "ok   mcuboot $id"
}

export -f conf_for strip_dts configure_app configure_mcuboot
export REPO OUT MODE

boards=("$@")
[[ ${#boards[@]} -eq 0 ]] && mapfile -t boards < <(list_boards)

case "$MODE" in
    snapshot|compare) ;;
    *) echo "unknown mode: $MODE" >&2; exit 2 ;;
esac

printf '%s\n' "${boards[@]}" | xargs -P "$JOBS" -I{} bash -c 'configure_app "$@"' _ {}
for id in "${boards[@]}"; do
    if [[ " $MCUBOOT_BOARDS " == *" $id "* ]]; then
        echo "$id"
    fi
done | xargs -r -P "$JOBS" -I{} bash -c 'configure_mcuboot "$@"' _ {}

[[ "$MODE" == snapshot ]] && exit 0

status=0
for id in "${boards[@]}"; do
    for f in config zephyr.dts mcuboot.dts; do
        [[ -f "$OUT/snapshot/$id/$f" ]] || continue
        if [[ ! -f "$OUT/compare/$id/$f" ]]; then
            echo "MISSING $id/$f"; status=1; continue
        fi
        if ! diff -u "$OUT/snapshot/$id/$f" "$OUT/compare/$id/$f" > "$OUT/compare/$id/$f.diff"; then
            echo "DIFF    $id/$f ($(grep -c '^[-+][^-+]' "$OUT/compare/$id/$f.diff") changed lines)"; status=1
        else
            rm -f "$OUT/compare/$id/$f.diff"
        fi
    done
    if [[ -d "$REPO/snippets/akira-board/boards" ]] && ! grep -q "Merged configuration '.*snippets/akira-board/boards/$id.conf'" "$OUT/compare/$id/app.log"; then
        echo "NOT MERGED $id: akira-board snippet conf was not applied"; status=1
    fi
done
[[ $status -eq 0 ]] && echo "All boards identical."
exit $status
