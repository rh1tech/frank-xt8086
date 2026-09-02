/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The state every emulated chip in src/chipset/ operates on.
 *
 * These are the registers of the parts an XT would have had as separate
 * packages — the PIC, the PIT, the DMA controller, the FDC, the UART, the
 * CRTC — plus the three memory arrays the 8086 actually addresses. They are
 * declared together because the port decoder in src/core/ports.h dispatches to
 * all of them from one switch, and because a chip model is only ever the
 * behaviour: the bytes it acts on live here, in one place, so a debugger or
 * a future save-state has a single object to walk.
 *
 * Split out of the old common.h, which mixed these with the board pin map
 * and a PSRAM bring-up routine. Board facts are in xt8086.h; this is state.
 */
#pragma once

#include "xt8086.h"

#include <stdint.h>

// ide_s holds an open FatFs handle for the hard-disk image.
#include "ff.h"

// The memory the 8086 actually addresses. RAM and the upper-memory block
// live in the external PSRAM (see src/drivers/psram) through the .psram linker
// section; video RAM stays in on-chip SRAM, because the scanline DMA reads
// it every line and a QSPI round trip per fetch is not a thing that ends
// well.
extern uint8_t UMB[UMB_SIZE] __attribute__((aligned(4)));
extern uint8_t RAM[RAM_SIZE] __attribute__((aligned(4)));
extern uint8_t VIDEORAM[VIDEORAM_SIZE] __attribute__((aligned(4)));

typedef struct {
    uint8_t interrupt_mask_register; //mask register
    uint8_t interrupt_request_register; //request register
    uint8_t in_service_register; //service register
    uint8_t initialization_command_word_step; //used during initialization to keep track of which ICW we're at
    uint8_t initialization_command_words_1; // ICW1
    uint8_t interrupt_vector_offset; //interrupt vector offset
    uint8_t register_read_mode; //remember what to return on read register from OCW3
} i8259_s;

typedef struct {
    uint16_t reload_value;       // Значение для загрузки в счётчик
    uint8_t access_mode;         // Режим доступа: LOBYTE/HIBYTE/TOGGLE
    uint8_t byte_toggle;         // Отслеживание байта в режиме TOGGLE
    uint8_t active;              // Канал активно считает (bool)
    uint8_t latch_mode;          // Режим latch: 0=нет, 1=lobyte, 2=hibyte, 3=toggle
    uint16_t latched_value;      // Защёлкнутое значение для LATCHCOUNT
    uint64_t start_timestamp_us; // Временная метка старта
    uint8_t operating_mode;
} i8253_channel_s;

// Intel 8253 Programmable Interval Timer (3 независимых канала)
typedef struct {
    i8253_channel_s channels[3];
} i8253_s;
typedef struct {
    uint32_t page;
    uint32_t address;
    uint32_t reload_address;
    uint32_t address_increase;
    uint16_t count;
    uint16_t reload_count;
    uint8_t auto_init;
    uint8_t mode;
    uint8_t enable;
    uint8_t masked;
    uint8_t dreq;
    uint8_t finished;
    uint8_t transfer_type;

    // Асинхронная передача данных (для polling на Core0)
    uint8_t data_source_type;
    const void *data_source;  // Источник данных (для device→memory)
    uint32_t data_offset;  // Смещениен в источнике
    uint8_t file_index;        // Индекс файла для FILE_READ/FILE_WRITE (0=fdd.img, 1=fdd1.img)
    uint8_t irq;               // IRQ для генерации при завершении (0 = нет IRQ)
    bool transfer_active;             // Флаг активной передачи
} dma_channel_s;

typedef struct {
    uint8_t rbr;           // Receive Buffer Register (текущий байт для чтения)
    uint8_t thr;           // Transmit Holding Register (для отправки)
    uint8_t ier;           // Interrupt Enable Register
    uint8_t iir;           // Interrupt Identification Register
    uint8_t lcr;           // Line Control Register (бит 7 = DLAB)
    uint8_t mcr;           // Modem Control Register
    uint8_t lsr;           // Line Status Register
    uint8_t msr;           // Modem Status Register
    uint16_t divisor;      // Divisor latch (для baud rate, игнорируем)
    bool data_ready;       // Флаг: есть данные в RBR для чтения

    // FIFO буфер для приема данных (для Microsoft Serial Mouse)
    uint8_t rx_fifo[16];   // Приемный FIFO (достаточно для нескольких mouse packets)
    uint8_t rx_head;       // Индекс головы (куда писать)
    uint8_t rx_tail;       // Индекс хвоста (откуда читать)
} uart_16550_s;

// Consolidated controller state for tighter locality; align to cache line for fast access
typedef struct {
    uint8_t DOR;
    uint8_t response[4];
    uint8_t command[9];
    uint8_t result[7];
    uint8_t presentCylinder[4];
    uint8_t command_length;
    uint8_t result_count;
    uint8_t command_index;
    uint8_t result_index;
    uint8_t check_drives_mask;
} i8272_s;

typedef struct {
    union {
        // Член для доступа к регистрам как к массиву
        uint8_t registers[16];

        // Член для доступа к регистрам по их именам
        struct {
            uint8_t h_total;            // R0: Horizontal Total
            uint8_t h_displayed;        // R1: Horizontal Displayed
            uint8_t h_sync_pos;         // R2: HSync Position
            uint8_t h_sync_width;       // R3: HSync Width
            uint8_t v_total;            // R4: Vertical Total
            uint8_t v_total_adjust;     // R5: VTotal Adjust
            uint8_t v_displayed;        // R6: Vertical Displayed
            uint8_t v_sync_pos;         // R7: VSync Position
            uint8_t interlace_mode;     // R8: Interlace Mode
            uint8_t max_scanline_addr;  // R9: Max Scanline Address
            uint8_t cursor_start;       // R10: Cursor Start Line
            uint8_t cursor_end;         // R11: Cursor End Line
            uint8_t start_addr_h;       // R12: Start Addr (H)
            uint8_t start_addr_l;       // R13: Start Addr (L)
            uint8_t cursor_addr_h;      // R14: Cursor Addr (H)
            uint8_t cursor_addr_l;      // R15: Cursor Addr (L)
        } r;
    };
    uint16_t vram_offset;

    bool cursor_blink_state;
    uint8_t text_blinking_mask;  // 0x80 = enabled, 0x00 = disabled (маска для бита атрибута)
    uint8_t cursor_x;
    uint8_t cursor_y;
} mc6845_s;

    typedef struct {
    uint8_t port3D8;
    uint8_t port3D9;
    uint8_t port3DA;
    uint8_t port3DA_tandy;

    /*
     * Tandy/PCjr page register, port 0x3DF, in 16K pages.
     *
     * crt_base is where the CRTC reads, cpu_base is what the processor
     * sees through the 32K window at B8000. Both zero at reset, which is
     * what a plain CGA leaves them as, so nothing changes for a machine
     * that never writes the register.
     */
    uint32_t tandy_crt_base;
    uint32_t tandy_cpu_base;
    bool updated;
} cga_s;


typedef struct {
    FIL* disk_image;
    uint8_t regs[8];
    uint8_t sector_buffer[512];
    uint16_t buffer_index;

    uint8_t high_byte;

    uint8_t current_command;
    uint16_t sectors_remaining;
} ide_s;

// Corrected CGA palette from https://int10h.org/blog/2022/06/ibm-5153-color-true-cga-palette/
constexpr uint32_t cga_palette[16] = {
    //R, G, B
    0x000000, // 0: black
    0x0000AA, // 1: blue
    0x00AA00, // 2: green
    0x00AAAA, // 3: cyan
    0xAA0000, // 4: red
    0xAA00AA, // 5: magenta
    0xAAAA00, // 6: brown. should be aa5500
    0xAAAAAA, // 7: light gray

    0x555555, // 8: dark gray
    0x0000FF, // 9: bright blue
    0x00FF00, // 10: bright green
    0x00FFFF, // 11: bright cyan
    0xFF0000, // 12: bright red
    0xFF00FF, // 13: bright magenta
    0xFFFF00, // 14: bright yellow
    0xFFFFFF  // 15: white
};

// Pallete, intensity, color_index from cga_palette
constexpr uint8_t cga_gfxpal[3][2][4] = {
    //palettes for 320x200 graphics mode
    {
        {0, 2, 4, 6}, //normal palettes
        {0, 10, 12, 14}, //intense palettes
    },
    {
            {0, 3, 5, 7},
            {0, 11, 13, 15},
        },
        {
            // the unofficial Mode 5 palette, accessed by disabling ColorBurst
            {0, 3, 4, 7},
            {0, 11, 12, 15},
        },
    };


