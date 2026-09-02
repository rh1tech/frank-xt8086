#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# psram — the ESP-PSRAM64H (U8) on XIP CS1.

add_library(psram INTERFACE)

target_sources(psram INTERFACE ${CMAKE_CURRENT_LIST_DIR}/psram.c)

target_include_directories(psram INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(psram INTERFACE pico_stdlib hardware_clocks)
