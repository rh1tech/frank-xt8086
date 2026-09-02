#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# sdcard — microSD on J4, over SPI1.
#
# The pin macros are still spelled SDCARD_PIN_SPI0_* upstream even though
# the driver has always driven SDCARD_SPI_BUS (spi1). Left alone rather
# than renamed, so the next update from upstream still applies; the top-
# level CMakeLists is where the real GP numbers are set.

if (NOT TARGET sdcard)
    add_library(sdcard INTERFACE)

    pico_generate_pio_header(sdcard ${CMAKE_CURRENT_LIST_DIR}/spi.pio)

    target_sources(sdcard INTERFACE
            ${CMAKE_CURRENT_LIST_DIR}/sdcard.c
            ${CMAKE_CURRENT_LIST_DIR}/pio_spi.c
    )

    target_include_directories(sdcard INTERFACE ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(sdcard INTERFACE
            fatfs pico_stdlib hardware_clocks hardware_spi hardware_pio)
endif()
