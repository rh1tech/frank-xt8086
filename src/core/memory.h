#pragma once
#include "state.h"

extern i8259_s i8259;   // state.c; POST masks it, see memory_read
#include "setup_menu.h"

// ============================================================================
// External Memory Arrays
// ============================================================================
extern uint8_t BIOS[];
extern uint8_t IDE[];
extern uint8_t XTROM[];
extern uint8_t VGABIOS[];

// Универсальная функция записи с поддержкой BHE (8/16-bit operations)
__always_inline static void write_to(uint8_t *destination, const uint32_t address,
                                       const uint16_t data, const bool bhe) {
    const uint32_t A0 = address & 1;

    // Fast path: aligned 16-bit write (90% случаев)
    if (likely(!(bhe | A0))) {
        *(uint16_t *)&destination[address] = data;
        return;
    }

    // Slow path: byte write
    const uint8_t byte_val = A0 ? data >> 8 : data & 0xFF;
    destination[address] = byte_val;
}

extern cga_s cga;   // state.c; the Tandy page register lives in it

/*
 * Top of conventional memory.
 *
 * 736K normally, which runs to 0xB8000 and leaves the CGA window above
 * it. Hercules puts its framebuffer at 0xB0000, inside that, so enabling
 * it lowers the ceiling to 0xB0000 and gives the 32K to the display
 * instead -- 704K of guest memory. Moving the limit rather than adding a
 * test keeps the cost off the hot path: every access already compares
 * against it.
 */
extern uint32_t ram_limit;

#include "vgacard.h"

// ============================================================================
// Memory Read (16-bit)
// ============================================================================
__force_inline static uint16_t memory_read(const uint32_t address, const bool bhe,
                                          const uint32_t a0) {
    /*
     * 0040:0072 is the warm-boot flag, and POST reading it is the one
     * moment we can recognise the BIOS starting up.
     *
     * It matters because of what GLaBIOS does four instructions later.
     * To save space it points SP at a table of return addresses inside
     * its own ROM -- `mov sp, 0E115h` at F000:E10F -- and a stack in ROM
     * works only while nothing can interrupt. Writes to it are discarded
     * and the matching reads come back as whatever byte the ROM holds.
     *
     * A hardware reset leaves the interrupt controller masked and the
     * timer stopped, so on a real machine nothing can. Reaching POST any
     * other way -- Ctrl+Alt+Del, or a guest that has lost its footing --
     * clears neither, and a timer tick landing in that window pushes a
     * return frame into nowhere and pops ROM bytes back in its place. The
     * flags come back with TF set and IF clear, and the machine spends
     * the rest of its life single-stepping one instruction with
     * interrupts off: running, drawing nothing, answering no key.
     *
     * Masking here is what the reset line would have done. POST programs
     * the controller for itself a moment later.
     */
    if (unlikely(address == 0x472u)) {
        i8259.interrupt_mask_register    = 0xFFu;
        i8259.interrupt_request_register = 0;
        i8259.in_service_register        = 0;
    }
    if (address < ram_limit) {
        return *(uint16_t *)&RAM[address];
    }

    /*
     * The text page belongs to the card when there is one.
     *
     * On a VGA the screen at 0xB8000 is the card's own memory -- the
     * character in plane 0, the attribute in plane 1 -- not a separate
     * buffer beside it. Routing it here is what lets the card keep all
     * 256K: there is no second copy to collide with, so the video BIOS
     * can load its font into plane 2 without landing on every other
     * character cell. The card works out which window it is answering
     * for from its own graphics controller registers.
     */
    if (settings.vga && (address - 0xA0000) < 0x20000) {
        const uint32_t coff = address - 0xA0000;
        if (likely(!(bhe | a0)))
            return (uint16_t)vgacard_mem_read(coff) |
                   (uint16_t)vgacard_mem_read(coff + 1) << 8;
        return a0 ? (uint16_t)vgacard_mem_read(coff + 1) << 8
                  : (uint16_t)vgacard_mem_read(coff);
    }

    if ((address - 0xB8000) < 0x8000) {
        // + the Tandy CPU page, which is zero unless something set it.
        return *(uint16_t *)&VIDEORAM[(cga.tandy_cpu_base + (address & 0x7FFF))
                                      & (VIDEORAM_SIZE - 1)];
    }

    // Only reachable once the ceiling has been lowered for Hercules.
    if ((address - 0xB0000) < 0x8000) {
        return *(uint16_t *)&VIDEORAM[HERC_VRAM_BASE + (address - 0xB0000)];
    }

    /*
     * The redundant twin of this block used to live here, unconditional
     * and reached only once VGA was disabled -- the settings.vga check
     * above already covers this whole range when the card is present, so
     * the only thing this one ever did was answer for the card after the
     * user had turned it off. Removed along with its write-side copy.
     */

    // The video BIOS, 32K at C000-C7FF. See tools/vgabios/README.md.
    //
    // Present only when settings.vga is on. GLaBIOS's C000 option-ROM
    // scan is memory-based, not settings-based: mapping this unconditionally
    // meant it found and ran the video BIOS regardless of the user's
    // choice, which programmed the card into VGA text mode while every
    // other path here -- the A0000 window above, the B8000 write below --
    // still treated the card as absent. Text kept its right shape,
    // because the geometry the renderer used did come from the card, but
    // every glyph was wrong: the video BIOS's writes went to the card and
    // the renderer's own text-page reads followed it there, while DOS's
    // writes went to the plain CGA buffer nothing was reading from
    // any more. With the ROM hidden, GLaBIOS never finds a video BIOS at
    // all and falls back to its own CGA INT 10h, exactly as if the
    // machine had no VGA card -- which is what the setting promises.
    if (settings.vga && (address - 0xC0000) < 0x8000) {
        return *(uint16_t *)&VGABIOS[address - 0xC0000];
    }

    if ((address - 0xC8000) < 8192) {
        return *(uint16_t *)&IDE[address - 0xC8000];
    }

    // Our option ROM. GLaBIOS scans C000-FDFF on 2K boundaries for the
    // 55 AA signature and far-calls what it finds, which is how this gets
    // to run inside the guest without anyone installing a file.
    if ((address - 0xD0000) < 2048) {
        return *(uint16_t *)&XTROM[address - 0xD0000];
    }

    // if ((address - 0xD0000) < UMB_SIZE) {
        // return *(uint16_t *)&UMB[address - 0xD0000];
    // }

    if (address == 0xFC000 && settings.tandy_enabled) {
        return 0x21; // Tandy signature
    }

    if (address >= BIOS_ROM_BASE) {
        // Патчим предпоследний байт BIOS для Tandy режима
        if (unlikely(address == 0xFFFFE && settings.tandy_enabled)) {
            return 0xFF; // TODO: IBM PC Jr = 0xFD, Tandy 1000 = 0xFF
        }
        return *(uint16_t *)&BIOS[address - BIOS_ROM_BASE];
    }

    // Unmapped memory
    bus_trace(0);
    return 0xFFFF;
}

// ============================================================================
// Memory Write (16-bit with BHE support)
// ============================================================================
__force_inline static void memory_write(const uint32_t address, const uint16_t data, const bool bhe) {
    if (address < ram_limit) {
        write_to(RAM, address, data, bhe);
        return;
    }

    if (settings.vga && (address - 0xA0000) < 0x20000) {
        const uint32_t coff = address - 0xA0000;
        const uint32_t cA0  = address & 1;
        if (likely(!(bhe | cA0))) {
            vgacard_mem_write(coff,     (uint8_t)data);
            vgacard_mem_write(coff + 1, (uint8_t)(data >> 8));
        } else {
            vgacard_mem_write(coff, cA0 ? (uint8_t)(data >> 8) : (uint8_t)data);
        }
        return;
    }

    if ((address - 0xB8000) < 0x8000) {
        write_to(VIDEORAM, (cga.tandy_cpu_base + (address & 0x7FFF))
                           & (VIDEORAM_SIZE - 1), data, bhe);
        return;
    }

    // Only reachable once the ceiling has been lowered for Hercules.
    if ((address - 0xB0000) < 0x8000) {
        write_to(VIDEORAM, HERC_VRAM_BASE + (address - 0xB0000), data, bhe);
        return;
    }

    // if ((address - 0xD0000) < UMB_SIZE) {
        // write_to(UMB, address - 0xD0000, data, bhe);
        // return;
    // }

    bus_trace(0);
    // ROM areas are read-only, ignore writes
}
