#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# vgacard — murm386's VGA, copied unchanged, plus the shim that attaches
# it to this machine's bus. See chipset/vgacard.h.
#
# STATIC rather than INTERFACE. An INTERFACE library's sources are
# compiled in the consumer's directory, where per-file properties set here
# are silently dropped -- which is how the OPL2 build once lost the flags
# it could not work without. A real target keeps its own.
#
add_library(vgacard STATIC
        ${CMAKE_CURRENT_LIST_DIR}/vga.c
        ${CMAKE_CURRENT_LIST_DIR}/vgacard.c
        ${CMAKE_CURRENT_LIST_DIR}/vgamodes.c
)

# Deliberately NOT propagated: murm386's vga.h would shadow this project's
# own. vga.c and vgacard.c reach it as a same-directory include, and
# everything else uses chipset/vgacard.h.
target_include_directories(vgacard PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${SRC_ROOT}/chipset)

# The RP2350 path is the one that pulls in pico/stdlib.h, which is where
# __not_in_flash_func comes from.
target_compile_definitions(vgacard PRIVATE RP2350_BUILD=1)

# FULL_UPDATE drops VGAState's last_ch_attr cache -- 132 * 60 words, very
# nearly 16K, and the whole reason vga_init's allocation was failing and
# taking the boot with it. It exists so vga_refresh() can redraw only the
# text cells that changed, and nothing here calls vga_refresh: this
# machine reads the card a scanline at a time and draws it itself.
target_compile_definitions(vgacard PRIVATE FULL_UPDATE=1)

# ...but its code goes in flash, not SRAM.
#
# murm386 marks these functions __not_in_flash_func because there they
# run on the video path. Here they do not: the card is reached from the
# bus interrupt, which is already the slowest thing on the machine, and
# XIP is cached. The SRAM they would occupy is worth far more as card
# memory -- a full 256K card is what lets a game park tile data in the
# upper half, which is exactly what Dangerous Dave 2 does.
target_compile_options(vgacard PRIVATE "-D__not_in_flash_func(x)=x")

# Another project's file, kept byte-identical so an upstream fix stays a
# copy rather than a merge. Its warnings are not ours to fix.
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/vga.c PROPERTIES COMPILE_FLAGS "-w")

target_link_libraries(vgacard PUBLIC pico_stdlib)
