#pragma once
#include "state.h"
#include "setup_menu.h"

// ============================================================================
// External Memory Arrays
// ============================================================================
extern uint8_t BIOS[];
extern uint8_t IDE[];

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

// ============================================================================
// Memory Read (16-bit)
// ============================================================================
__force_inline static uint16_t memory_read(const uint32_t address) {
    if (address < RAM_SIZE) {
        return *(uint16_t *)&RAM[address];
    }

    if ((address - 0xB8000) < 0x8000) {
        return *(uint16_t *)&VIDEORAM[address & 0x7FFF];
    }

    if ((address - 0xC8000) < 8192) {
        return *(uint16_t *)&IDE[address - 0xC8000];
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
    if (address < RAM_SIZE) {
        write_to(RAM, address, data, bhe);
        return;
    }

    if ((address - 0xB8000) < 0x8000) {
        write_to(VIDEORAM, address & 0x7FFF, data, bhe);
        return;
    }

    // if ((address - 0xD0000) < UMB_SIZE) {
        // write_to(UMB, address - 0xD0000, data, bhe);
        // return;
    // }

    bus_trace(0);
    // ROM areas are read-only, ignore writes
}
