cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)
project(apple1_pico C CXX ASM)
pico_sdk_init()

add_executable(apple1_pico
    main.c
    hw_config.c
    aci.c
    bus_driver.c
)

pico_generate_pio_header(apple1_pico ${CMAKE_CURRENT_LIST_DIR}/bus_control.pio)

pico_enable_stdio_usb(apple1_pico 1)
pico_enable_stdio_uart(apple1_pico 0)
pico_add_extra_outputs(apple1_pico)
target_link_libraries(apple1_pico pico_stdlib hardware_pio hardware_pwm hardware_adc)
