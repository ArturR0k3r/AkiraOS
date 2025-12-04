# @copyright Copyright © contributors to Project Ocre,
# which has been established as Project Ocre a Series of LF Projects, LLC
# SPDX-License-Identifier: Apache-2.0
#
# AkiraOS Hello World WASM App - Based on OCRE SDK patterns

cmake_minimum_required(VERSION 3.20.0)

# Set the WASI SDK toolchain file
set(CMAKE_TOOLCHAIN_FILE /opt/wasi-sdk/share/cmake/wasi-sdk.cmake)

# Set the project name (needs to come after the toolchain file) 
set(APPNAME hello-world)
project(${APPNAME})

# Set the linker flags - optimized for embedded targets with memory constraints
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--strip-all -Wl,--allow-undefined")

# Set compilation flags
add_compile_options(
    -O3                         # Maximum optimization
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-unknown-attributes
)

# Memory constraints for embedded targets
# 64KB is the minimum WASM page size and works well with constrained WAMR heaps
add_link_options(
    -z stack-size=4096          # 4KB stack should be plenty for hello world
    -Wl,--initial-memory=65536  # Minimum size of linear memory (1 page = 64KB)
    -Wl,--max-memory=65536      # Maximum size of linear memory (1 page = 64KB)
)

# Create the executable target
add_executable(${APPNAME}.wasm main.c)
