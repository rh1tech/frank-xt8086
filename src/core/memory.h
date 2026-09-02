#pragma once
#include "state.h"
#include "setup_menu.h"

// ============================================================================
// External Memory Arrays
// ============================================================================
extern uint8_t BIOS[];
extern uint8_t IDE[];
extern uint8_t XTROM[];

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

// ============================================================================
// Memory Read (16-bit)
// ============================================================================
__force_inline static uint16_t memory_read(const uint32_t address) {
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

    // if ((address - 0xD0000) < UMB_SIZE) {
        // write_to(UMB, address - 0xD0000, data, bhe);
        // return;
    // }

    bus_trace(0);
    // ROM areas are read-only, ignore writes
}
