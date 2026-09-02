#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# vga — six-bit R2R ladder plus separate H/V sync on GP30-GP37.
#
# VGA_BASE_PIN and VGA_CSYNC are set by the top-level CMakeLists from the
# board's pin map rather than defaulted here, so there is one place where
# the ladder's position is written down.

add_library(vga INTERFACE)

target_sources(vga INTERFACE ${CMAKE_CURRENT_LIST_DIR}/vga.c)

# The chipset state lives in core/ and this driver reaches into it — the
# CGA/Tandy mode registers here, the emulated COM1 in usbhid. Adding the
# directories rather than linking xt8086_core, which would be circular.
target_include_directories(vga INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../../core
        ${CMAKE_CURRENT_LIST_DIR}/../../chipset
        ${CMAKE_CURRENT_LIST_DIR}/../fatfs)

target_link_libraries(vga INTERFACE hardware_pio hardware_dma)
