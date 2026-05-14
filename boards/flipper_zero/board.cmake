# Copyright (c) 2026 Akira Project
# SPDX-License-Identifier: Apache-2.0
#
# Flipper Zero board runner — USB DFU (primary) + OpenOCD/SWD (fallback)
#
# ============================================================================
# Primary: dfu-util via USB ROM DFU bootloader
# ============================================================================
# To enter ROM DFU mode on Flipper Zero:
#   1. Connect USB cable
#   2. Hold BACK button while pressing RESET (or power-cycle with BACK held)
#   3. Flipper Zero appears as "STM32 BOOTLOADER" (VID=0483 PID=DF11)
#
# STM32WB55 ROM DFU places the application at 0x08000000.
# The --dfuse-address argument includes ":leave" to auto-boot after flash.
# ============================================================================
board_runner_args(dfu-util
  "--pid=0483:df11"
  "--alt=0"
  "--dfuse-address=0x08000000:leave"
)
include(${ZEPHYR_BASE}/boards/common/dfu-util.board.cmake)

# ============================================================================
# Fallback: OpenOCD via ST-Link v2 (SWD — 4-pin header on Flipper Zero PCB)
# ============================================================================
# Uncomment to use ST-Link / SWD instead of USB DFU:
#
# board_runner_args(openocd
#   "--config" "interface/stlink.cfg"
#   "--config" "target/stm32wbx.cfg"
# )
# include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
