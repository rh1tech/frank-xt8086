#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# opl2 — the YM3812 the AdLib card carried.
#
# emu8950, vendored from pico-286, which forked it from Digital Sound
# Antiques. Includes the hand-written Cortex-M33 slot renderer, which
# targets exactly this chip.
#
# A STATIC library, unlike every other driver here, and deliberately.
# The others are INTERFACE because they need the board's pin numbers from
# the parent scope; this one needs nothing from us and requires two things
# our own code should never inherit:
#
#   -fms-extensions   OPL_SLOT embeds `struct SLOT_RENDER;` as an
#                     anonymous member -- a *named* struct type with no
#                     member name, which is a Microsoft extension.
#                     Standard C allows anonymous members only for unnamed
#                     structs, so without this every field SLOT_RENDER
#                     contributes is invisible and the build fails with
#                     dozens of "OPL_SLOT has no member named patch".
#
#   its own warnings  vendored code, whose warnings are not ours to act on.
#
# As an INTERFACE library these sources compile in the *executable's*
# directory scope, where set_source_files_properties() from this file does
# not reach -- the flags were silently dropped and the anonymous-member
# errors came back with no sign of why. A static library compiles here,
# where its own properties apply.

add_library(opl2 STATIC
        emu8950.c
        emuadpcm.c
        slot_render.cpp
        slot_render_pico.S)

target_include_directories(opl2 PUBLIC ${CMAKE_CURRENT_LIST_DIR})

# PUBLIC: the struct layout depends on these, so anything that includes
# emu8950.h must see exactly the same set or its idea of OPL_SLOT will
# disagree with the library's.
target_compile_definitions(opl2 PUBLIC
        USE_EMU8950_OPL
        EMU8950_SLOT_RENDER=1
        EMU8950_NO_RATECONV=1
        EMU8950_NO_WAVE_TABLE_MAP=1
        EMU8950_NO_TLL=1
        EMU8950_NO_FLOAT=1
        EMU8950_NO_TIMER=1
        EMU8950_NO_TEST_FLAG=1
        EMU8950_SIMPLER_NOISE=1
        EMU8950_SHORT_NOISE_UPDATE_CHECK=1
        EMU8950_LINEAR_SKIP=1
        EMU8950_LINEAR_END_OF_NOTE_OPTIMIZATION
        EMU8950_NO_PERCUSSION_MODE=1
        EMU8950_LINEAR=1)

# PUBLIC too: our own adlib.c includes emu8950.h and hits the same
# anonymous member.
target_compile_options(opl2 PUBLIC -fms-extensions)
target_compile_options(opl2 PRIVATE -Wno-all -Wno-unused-variable -Wno-unused-function)

# hardware_interp: the ARM renderer uses the RP2350's interpolators for
# the envelope table lookups, which is most of why it is fast.
target_link_libraries(opl2 PUBLIC pico_stdlib hardware_interp)
