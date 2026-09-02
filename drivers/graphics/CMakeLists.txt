#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# graphics — CGA/Tandy mode rendering and the text-mode fonts, on top of
# whichever scanline backend is linked (vga, here).

add_library(graphics INTERFACE)

target_sources(graphics INTERFACE ${CMAKE_CURRENT_LIST_DIR}/graphics.c)

# The chipset state lives in core/ and this driver reaches into it — the
# CGA/Tandy mode registers here, the emulated COM1 in usbhid. Adding the
# directories rather than linking xt8086_core, which would be circular.
target_include_directories(graphics INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../../core
        ${CMAKE_CURRENT_LIST_DIR}/../../chipset
        ${CMAKE_CURRENT_LIST_DIR}/../fatfs)
