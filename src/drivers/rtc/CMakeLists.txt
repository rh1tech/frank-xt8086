#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# rtc — the DS3231MZ (U15) on I2C1, GP38/GP39.

add_library(rtc INTERFACE)

target_sources(rtc INTERFACE ${CMAKE_CURRENT_LIST_DIR}/ds3231.c)

target_include_directories(rtc INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../../core)   # xt8086.h, for the pin numbers

target_link_libraries(rtc INTERFACE pico_stdlib hardware_i2c)
