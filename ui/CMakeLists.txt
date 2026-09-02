#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ui — SETUP, the file browser, and the serial console.
#
# console.h is header-only and stays that way: its screen mirror runs off
# main()'s frame tick and wants to inline there.

add_library(xt8086_ui INTERFACE)

target_sources(xt8086_ui INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/setup_menu.c
)

target_include_directories(xt8086_ui INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(xt8086_ui INTERFACE xt8086_core graphics fatfs usbhid)
