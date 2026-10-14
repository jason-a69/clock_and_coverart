# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# As far as I know the Raspberry Pi Pico plugin for VSCode does not (yet) support bare RP2350 microcontrollers without a board.
# To adapt the C-SDK for a standard Pico board configure this CMakeLists.txt file via the Raspberry Pi Pico plugin's "Switch Board" option.
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Include the local secrets if they exist
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake.secret")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake.secret)
else()
    # Provide dummy values so the build doesn't break on GitHub/CI
    set(SECRET_WIFI_SSID "PLACEHOLDER_WIFI_SSID")
    set(SECRET_WIFI_PASSWORD "PLACEHOLDER_WIFI_PASSWORD")
endif()

# Tell CMake to generate the actual header file inside the build directory
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Config.h"
)

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(clock_and_coverart C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_subdirectory(utils)

# Add library. This is the core HUB75 driver.
add_library(hub75 STATIC)

target_sources(hub75 PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src/hub75.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/icnd2153.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/rul6024.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/fm6126a.cpp
)

target_link_libraries(hub75 PUBLIC
        pico_stdlib
        pico_multicore
        pico_aon_timer
        hardware_pio
        hardware_dma
        hardware_vreg
        pico_graphics
)

target_include_directories(hub75 PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_include_directories(hub75 PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
)

target_include_directories(hub75 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

pico_generate_pio_header(hub75 
        ${CMAKE_CURRENT_LIST_DIR}/src/hub75.pio
        ${CMAKE_CURRENT_LIST_DIR}/src/icnd2153.pio)

# target_compile_definitions are PUBLIC so any target that links "hub75" (e.g. hub75_demo or hub75_demo_dual below, or a
# consumer project's own executable) automatically inherits the exact same panel configuration.
target_compile_definitions(hub75 PUBLIC
    USE_PICO_GRAPHICS=true   # set to false if you use hub75 as a library - any reference to pico_graphics is removed
)

# Add demo executable. Everything that is only required for the demo remains here.
add_executable(clock_and_coverart clock_and_coverart.cpp )

pico_set_program_name(clock_and_coverart "clock_and_coverart")
pico_set_program_version(clock_and_coverart "1.0")

pico_set_float_implementation(clock_and_coverart pico)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(clock_and_coverart 0)
pico_enable_stdio_usb(clock_and_coverart 1)

add_subdirectory(common)
add_subdirectory(libraries)

target_link_libraries(clock_and_coverart PRIVATE
                        hub75)

target_link_libraries(clock_and_coverart PRIVATE
                        pico_cyw43_arch_lwip_threadsafe_background
                        pico_lwip_sntp)  # LWIP sntp application


pico_add_extra_outputs(clock_and_coverart)
print_size_info(clock_and_coverart)


