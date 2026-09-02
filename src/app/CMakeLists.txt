#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The top of the build. Configure with `cmake -S src/app -B build`.
#
cmake_minimum_required(VERSION 3.13)

# Two roots, because two different things live at them. FW_ROOT is the
# repository — where boards/, roms/ and version.txt are, and the -I the
# assembler needs to resolve the .incbin paths in src/core/state.c.
# SRC_ROOT is the code.
set(SRC_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(FW_ROOT  ${CMAKE_CURRENT_LIST_DIR}/../..)

# boards/ stays at the repository root rather than under src/: this is the
# path the SDK's board-header convention expects, and it is configuration
# for the build, not code the firmware compiles.
set(PICO_BOARD_HEADER_DIRS ${FW_ROOT}/boards CACHE PATH "" FORCE)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "frank_xt8086" CACHE STRING "" FORCE)
endif()
set(PICO_PLATFORM rp2350-arm-s)

# Version, from version.txt at the repo root: "MAJOR MINOR", rendered as
# MAJOR.MM. One file is the single source of truth for the banner and for
# release artefact names, so a release cannot ship a binary whose reported
# version disagrees with its filename.
file(READ ${FW_ROOT}/version.txt _ver_raw)
string(STRIP "${_ver_raw}" _ver_raw)
string(REPLACE " " ";" _ver_list "${_ver_raw}")
list(GET _ver_list 0 _ver_major)
list(GET _ver_list 1 _ver_minor)
string(LENGTH "${_ver_minor}" _ver_minor_len)
if(_ver_minor_len LESS 2)
    set(_ver_minor "0${_ver_minor}")
endif()
set(XT8086_VERSION "${_ver_major}.${_ver_minor}")

include(${FW_ROOT}/pico_sdk_import.cmake)

project(frank-xt8086 C CXX ASM)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)

pico_sdk_init()

message(STATUS "frank-xt8086 ${XT8086_VERSION} for ${PICO_BOARD}")

# ---------------------------------------------------------------------------
# Knobs
# ---------------------------------------------------------------------------
#
# CPU_SPEED is the one worth knowing about. 504 MHz is what the prototype
# ran at and needs the voltage regulator pushed to 1.65 V; 252 MHz is the
# clock this board's own self-test firmware settles on and needs no such
# push. Every derived rate — the PIO pixel clock, the QMI divisor, the
# 8086's own clock — is computed from clk_sys at runtime, so both are
# correct builds and the difference is margin, not function.
set(CPU_SPEED   504 CACHE STRING "System clock, MHz")
set(PSRAM_SPEED 166 CACHE STRING "Ceiling for the QSPI PSRAM clock, MHz")

# The screen mirror and keystroke injection over UART0. Useful with a probe
# attached, expensive when nobody is reading: it repaints 2000 characters
# every frame. See src/ui/console.h.
option(SERIAL_CONSOLE "Mirror the text screen over UART0 and accept keys" OFF)

# Bring-up only: a pin that goes low on an access to unmapped address
# space. Unset by default because this board has no pin to spare — see
# src/core/xt8086.h.
set(BUS_TRACE_PIN "" CACHE STRING "GPIO for the unmapped-access marker")

# Bring-up only: the prototype's 500 Hz-5 kHz speaker sweep, for finding a
# new transducer's resonance by ear. Holds the boot for ~1.5 s.
option(BEEPER_SWEEP "Replace the boot chime with a frequency sweep" OFF)

# ---------------------------------------------------------------------------
# Components
# ---------------------------------------------------------------------------
add_subdirectory(${SRC_ROOT}/drivers/fatfs    src/drivers/fatfs)
add_subdirectory(${SRC_ROOT}/drivers/sdcard   src/drivers/sdcard)
add_subdirectory(${SRC_ROOT}/drivers/graphics src/drivers/graphics)
add_subdirectory(${SRC_ROOT}/drivers/vga      src/drivers/vga)
add_subdirectory(${SRC_ROOT}/drivers/usbhid   src/drivers/usbhid)
add_subdirectory(${SRC_ROOT}/drivers/psram    src/drivers/psram)
add_subdirectory(${SRC_ROOT}/core             core)
add_subdirectory(${SRC_ROOT}/ui               ui)

add_executable(frank-xt8086 main.c)

# The 8086 bus controller. Generated into src/core/ so an editor resolves
# "i8086_bus.pio.h" the same way the compiler does.
pico_generate_pio_header(frank-xt8086
        ${SRC_ROOT}/core/i8086_bus.pio
        OUTPUT_DIR ${SRC_ROOT}/core)

# ---------------------------------------------------------------------------
# Configuration reaching the code
# ---------------------------------------------------------------------------
target_compile_definitions(frank-xt8086 PRIVATE
        FIRMWARE_NAME="frank-xt8086"
        FIRMWARE_VERSION="${XT8086_VERSION}"

        CPU_SPEED_MHZ=${CPU_SPEED}
        PSRAM_SPEED_MHZ=${PSRAM_SPEED}

        # The video ladder, from src/core/xt8086.h. VGA_CSYNC is 0 because this
        # board brings H and V out separately — GP36 and GP37 go both to the
        # VGA connector's pins 13 and 14 and to the MS9288A's HSIN/VSIN, so
        # there is nothing to fold onto green.
        VGA
        VGA_BASE_PIN=30
        VGA_CSYNC=0

        # microSD on J4. The macro names say SPI0; the driver has always
        # used spi1. See src/drivers/sdcard/CMakeLists.txt.
        SDCARD_PIN_SPI0_MISO=40
        SDCARD_PIN_SPI0_CS=41
        SDCARD_PIN_SPI0_SCK=42
        SDCARD_PIN_SPI0_MOSI=43

        PICO_FLASH_SIZE_BYTES=16777216
        PICO_PRINTF_ALWAYS_INCLUDED=0
        PICO_PANIC_FUNCTION=0
)

if(SERIAL_CONSOLE)
    target_compile_definitions(frank-xt8086 PRIVATE SERIAL_CONSOLE=1)
endif()

if(BUS_TRACE_PIN)
    target_compile_definitions(frank-xt8086 PRIVATE BUS_TRACE_PIN=${BUS_TRACE_PIN})
endif()

if(BEEPER_SWEEP)
    target_compile_definitions(frank-xt8086 PRIVATE BEEPER_SWEEP=1)
endif()

# TinyUSB host wants these names; the SDK's own examples set them the same
# way. Only meaningful because usbhid is unconditional now.
set(FAMILY rp2350)
set(BOARD pico_sdk)

target_include_directories(frank-xt8086 PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${FW_ROOT})

target_link_libraries(frank-xt8086 PRIVATE
        xt8086_core
        xt8086_ui
        vga
        sdcard

        pico_runtime
        pico_stdlib
        pico_stdio
        pico_multicore
        tinyusb_host
        tinyusb_board
        hardware_pio
        hardware_pwm
)

# ---------------------------------------------------------------------------
# Console
# ---------------------------------------------------------------------------
#
# UART only. The USB controller is a HID host so a keyboard and mouse can
# be plugged in, and it cannot simultaneously enumerate as a CDC device.
# That was a real cost on the prototype board, which had no UART header;
# here J1 is right there and there is no trade to make.
pico_enable_stdio_uart(frank-xt8086 1)
pico_enable_stdio_usb(frank-xt8086 0)

# ---------------------------------------------------------------------------
# Image
# ---------------------------------------------------------------------------
#
# copy_to_ram, not XIP: the two bus interrupt handlers must answer the 8086
# within a wait state, and a flash fetch that misses the XIP cache does not.
pico_set_binary_type(frank-xt8086 copy_to_ram)
pico_set_linker_script(frank-xt8086 ${SRC_ROOT}/app/memmap.ld)

pico_set_program_name(frank-xt8086 "frank-xt8086")
pico_set_program_version(frank-xt8086 "${XT8086_VERSION}")

target_compile_options(frank-xt8086 PRIVATE -Ofast -Wall -Wno-unused-parameter)

target_link_options(frank-xt8086 PRIVATE
        -Xlinker --print-memory-usage
        -Wl,--gc-sections)

pico_add_extra_outputs(frank-xt8086)
