#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usbhid — TinyUSB in host mode: a USB keyboard becomes XT scancodes, a
# USB mouse becomes Microsoft Serial Mouse packets on the emulated COM1.
#
# Always built now. On the prototype the USB controller had to choose
# between being a HID host and being the console's CDC device; this board
# has a real UART on J1, so the console never asks for USB and the host
# side is unconditional.

add_library(usbhid INTERFACE)

target_sources(usbhid INTERFACE ${CMAKE_CURRENT_LIST_DIR}/hid_app.c)

# The chipset state lives in core/ and this driver reaches into it — the
# CGA/Tandy mode registers here, the emulated COM1 in usbhid. Adding the
# directories rather than linking xt8086_core, which would be circular.
target_include_directories(usbhid INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../../core
        ${CMAKE_CURRENT_LIST_DIR}/../../chipset
        ${CMAKE_CURRENT_LIST_DIR}/../fatfs)

target_link_libraries(usbhid INTERFACE
        hardware_pio hardware_clocks tinyusb_host tinyusb_board)
