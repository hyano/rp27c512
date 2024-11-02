# Copyright (c) 2024 Hirokuni Yano
#
# Released under the MIT license.
# see https://opensource.org/licenses/MIT
#
cmake_minimum_required(VERSION 3.13)

#set(PICO_PLATFORM rp040)
#set(PICO_BOARD pico)

include(pico_sdk_import.cmake)

project(rp27c512 C CXX ASM)
set(CMAKE_C_STNDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

set(PICO_DEFAULT_BINARY_TYPE copy_to_ram)

add_executable(rp27c512
  rp27c512.c
  romemu.c
  busmon.c
  microrl-remaster/src/microrl/microrl.c
)

pico_generate_pio_header(rp27c512 ${CMAKE_CURRENT_SOURCE_DIR}/romemu.pio)
pico_generate_pio_header(rp27c512 ${CMAKE_CURRENT_SOURCE_DIR}/busmon.pio)

pico_enable_stdio_usb(rp27c512 1)
pico_enable_stdio_uart(rp27c512 0)

pico_add_extra_outputs(rp27c512)

add_subdirectory(xmodem)

target_include_directories(rp27c512 PRIVATE . ./microrl-remaster/src/include/microrl)
target_link_libraries(rp27c512 pico_stdlib pico_multicore hardware_pio hardware_dma hardware_flash hardware_sync hardware_watchdog xmodem)

pico_define_boot_stage2(slower_boot2 ${PICO_DEFAULT_BOOT_STAGE2_FILE})
target_compile_definitions(slower_boot2 PRIVATE PICO_FLASH_SPI_CLKDIV=4)
pico_set_boot_stage2(rp27c512 slower_boot2)
