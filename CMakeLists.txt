cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(AkiraOS LANGUAGES C)

# Source files
set(SRC_FILES
  src/main.c
  src/display.c
)

# Include paths
zephyr_include_directories(src)

# Build target
target_sources(app PRIVATE ${SRC_FILES})
