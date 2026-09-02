#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# audio — the PWM DAC on GP46, and the mixer feeding it.

add_library(audio INTERFACE)

target_sources(audio INTERFACE ${CMAKE_CURRENT_LIST_DIR}/audio.c)

target_include_directories(audio INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../../core        # xt8086.h, for BEEPER_PIN
        ${CMAKE_CURRENT_LIST_DIR}/../../chipset)    # adlib.h

target_link_libraries(audio INTERFACE
        opl2 pico_stdlib hardware_pwm hardware_dma hardware_clocks)
