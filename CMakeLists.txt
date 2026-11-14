cmake_minimum_required(VERSION 3.13)

# Pico SDK (set PICO_SDK_PATH in your environment or with -DPICO_SDK_PATH=...)
if (NOT PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(apple1_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# SD card support: carlk3's no-OS-FatFS-SD-SDIO-SPI-RPi-Pico (FatFs over SPI/SDIO).
# Supports RP2040 (Pico) and RP2350 (Pico 2).
#   git submodule add https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
# (or point FATFS_DIR at a copy you already have)
set(FATFS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico" CACHE PATH
    "no-OS-FatFS-SD-SDIO-SPI-RPi-Pico checkout")
if (NOT EXISTS "${FATFS_DIR}/src/CMakeLists.txt")
    message(FATAL_ERROR "FatFs SD library not found in ${FATFS_DIR} - see comment above")
endif()
add_subdirectory(${FATFS_DIR}/src build_fatfs)

add_executable(apple1 main.c aci.c hw_config.c)
target_include_directories(apple1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(apple1 PRIVATE -O2 -Wall)
#target_link_libraries(apple1 pico_stdlib no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
target_link_libraries(apple1 pico_stdlib hardware_adc no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)

# Console over USB CDC, not UART
pico_enable_stdio_usb(apple1 1)
pico_enable_stdio_uart(apple1 0)

pico_add_extra_outputs(apple1)
