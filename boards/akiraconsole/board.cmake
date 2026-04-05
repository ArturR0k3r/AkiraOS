# SPDX-License-Identifier: Apache-2.0

if(NOT "${OPENOCD}" MATCHES "^${ESPRESSIF_TOOLCHAIN_PATH}/.*")
  set(OPENOCD OPENOCD-NOTFOUND)
endif()
find_program(OPENOCD openocd PATHS ${ESPRESSIF_TOOLCHAIN_PATH}/openocd-esp32/bin NO_DEFAULT_PATH)
include(${ZEPHYR_BASE}/boards/common/esp32.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)

# MCUboot slot0_partition is at 0x20000. The esp32 runner defaults to 0x10000
# (sys_partition), which is wrong for MCUboot builds. Override here so that
# "west flash" always writes zephyr.signed.bin to the correct slot.
board_runner_args(esp32 "--esp-app-address=0x20000")

# Flash geometry for ESP32-S3 N16R8 (16 MB, DIO, 40 MHz).
board_runner_args(esp32 "--esp-flash-size=16MB")
board_runner_args(esp32 "--esp-flash-mode=dio")
board_runner_args(esp32 "--esp-flash-freq=40m")

# WARNING: Do NOT pass --erase to "west flash".
# "west flash --erase" performs a full chip erase and will destroy the NVS
# settings partition (0x420000) and the LittleFS app partition (0x428000).
# Normal "west flash" only writes the app image into slot0 (0x20000-0x21FFFF)
# and never touches the data region above 0x420000.
