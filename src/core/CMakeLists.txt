#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# core — the bus, the clock, the memory map, the port map, the state.
#
# An INTERFACE library rather than a static one, because most of what is
# here is header-only by necessity: memory.h and ports.h are on the read
# and write paths of every 8086 bus cycle and have to inline into the two
# PIO interrupt handlers in cpu_bus.c. A library boundary between them
# would be a function call per cycle at six megahertz.

add_library(xt8086_core INTERFACE)

target_sources(xt8086_core INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/cpu_bus.c
        ${CMAKE_CURRENT_LIST_DIR}/cpu_clock.c
        ${CMAKE_CURRENT_LIST_DIR}/cpu_probe.c
        ${CMAKE_CURRENT_LIST_DIR}/state.c
        ${CMAKE_CURRENT_LIST_DIR}/../chipset/mc146818.c
        ${CMAKE_CURRENT_LIST_DIR}/../chipset/adlib.c
)

target_include_directories(xt8086_core INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../chipset
)

# ports.h reaches into the emulated parts, the video driver and the SD
# filesystem; propagating those here is what lets a consumer include
# "ports.h" and nothing else.
target_link_libraries(xt8086_core INTERFACE
        graphics
        fatfs
        rtc
        opl2
        usbhid
        psram
        pico_stdlib
        hardware_pio
        hardware_pwm
)
