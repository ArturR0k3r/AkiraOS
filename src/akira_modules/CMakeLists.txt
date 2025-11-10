# SPDX-License-Identifier: Apache-2.0
# Akira Module System - CMake Configuration

if(CONFIG_AKIRA_MODULE)

zephyr_library()

# Core Akira Module API
zephyr_library_sources(
    src/akira_module_core.c
    src/akira_module_registry.c
    src/akira_module_manager.c
)

# Communication interfaces
if(CONFIG_AKIRA_MODULE_COMM_UART)
    zephyr_library_sources(src/comm/akira_comm_uart.c)
endif()

if(CONFIG_AKIRA_MODULE_COMM_SPI)
    zephyr_library_sources(src/comm/akira_comm_spi.c)
endif()

if(CONFIG_AKIRA_MODULE_COMM_I2C)
    zephyr_library_sources(src/comm/akira_comm_i2c.c)
endif()

if(CONFIG_AKIRA_MODULE_COMM_NETWORK)
    zephyr_library_sources(src/comm/akira_comm_network.c)
endif()

# Protocol handlers
if(CONFIG_AKIRA_MODULE_PROTOCOL_JSON)
    zephyr_library_sources(src/protocol/akira_protocol_json.c)
endif()

if(CONFIG_AKIRA_MODULE_PROTOCOL_MSGPACK)
    zephyr_library_sources(src/protocol/akira_protocol_msgpack.c)
endif()

# Include directories
zephyr_library_include_directories(
    include
    ${ZEPHYR_BASE}/include
)

# Export include directories for applications
zephyr_include_directories(include)

endif() # CONFIG_AKIRA_MODULE
