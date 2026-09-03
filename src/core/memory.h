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

    if ((address - 0xB8000) < 0x8000) {
        // + the Tandy CPU page, which is zero unless something set it.
        return *(uint16_t *)&VIDEORAM[(cga.tandy_cpu_base + (address & 0x7FFF))
                                      & (VIDEORAM_SIZE - 1)];
    }

    // Only reachable once the ceiling has been lowered for Hercules.
    if ((address - 0xB0000) < 0x8000) {
        return *(uint16_t *)&VIDEORAM[HERC_VRAM_BASE + (address - 0xB0000)];
    }

    // The EGA/VGA card owns 0xA0000..0xAFFFF, and decides for itself what
    // a write there means: which planes, through which latches, under
    // which write mode. See chipset/vgacard.h.
    if ((address - 0xA0000) < 0x10000) {
        /*
         * Byte enables matter on a read here, which they do nowhere else.
         *
         * Reading EGA memory is not a passive act: every read loads the
         * card's four latches from the address read. This used to fetch
         * both halves of the word whatever the CPU had asked for, so a
         * byte read of an even address went on to read the odd one too
         * and left the latches holding the *next* byte.
         *
         * Latch-based drawing -- write mode 1, and the read-modify-write
         * that masked plane writes amount to -- then wrote the wrong
         * eight pixels. It showed as vertical bands one byte wide
         * straight through the picture, which is how Dangerous Dave 2's
         * title came out striped.
         */
        const uint32_t off = address - 0xA0000;
        if (likely(!(bhe | a0)))
            return (uint16_t)vgacard_mem_read(off) |
                   (uint16_t)vgacard_mem_read(off + 1) << 8;
        return a0 ? (uint16_t)vgacard_mem_read(off + 1) << 8
                  : (uint16_t)vgacard_mem_read(off);
    }

    // The video BIOS, 32K at C000-C7FF. See tools/vgabios/README.md.
    if ((address - 0xC0000) < 0x8000) {
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

    if ((address - 0xA0000) < 0x10000) {
        /*
         * The same byte-enable rules write_to() uses, and they matter
         * more here: every one of these goes through the card's write
         * logic, which may spread one byte across four planes. Getting
         * the wrong half of a word to the wrong address does not smear
         * the picture, it fills it with rubbish.
         */
        const uint32_t off = address - 0xA0000;
        const uint32_t A0  = address & 1;
        if (likely(!(bhe | A0))) {
            vgacard_mem_write(off,     (uint8_t)data);
            vgacard_mem_write(off + 1, (uint8_t)(data >> 8));
        } else {
            vgacard_mem_write(off, A0 ? (uint8_t)(data >> 8) : (uint8_t)data);
        }
        return;
    }

    // if ((address - 0xD0000) < UMB_SIZE) {
        // write_to(UMB, address - 0xD0000, data, bhe);
        // return;
    // }

    bus_trace(0);
    // ROM areas are read-only, ignore writes
}
