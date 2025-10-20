# Akira Console SDL2 Simulator Module
# Only built for native_sim platform

if(NOT CONFIG_BOARD_NATIVE_SIM)
    return()
endif()

# Find SDL2 using pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# Add simulator sources
target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/akira_sim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/akira_sim_display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/akira_sim_buttons.c
)

# Include SDL2 headers
target_include_directories(app PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link SDL2 libraries
target_link_libraries(app PRIVATE
    ${SDL2_LIBRARIES}
    m
    pthread
)

# Add link directories for SDL2
target_link_directories(app PRIVATE
    ${SDL2_LIBRARY_DIRS}
)

# Add compile definitions
target_compile_definitions(app PRIVATE
    AKIRA_SIM_ENABLED=1
)

message(STATUS "✅ Akira Console SDL2 Simulator enabled")
message(STATUS "   SDL2 include dirs: ${SDL2_INCLUDE_DIRS}")
message(STATUS "   SDL2 libraries: ${SDL2_LIBRARIES}")
