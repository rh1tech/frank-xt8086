#pragma once
#include <stdio.h>


#include "state.h"

// Tracing the CRTC is useful exactly once — when a BIOS programs a mode the
// video driver does not expect — and ruinous every other time.
//
// port_write() runs inside the PIO write handler, at the highest interrupt
// priority, with the 8086 held in a wait state until it returns. One
// forty-character line at 115200 baud is about 3.5 ms of stalled CPU, and
// programming a mode writes sixteen registers back to back.
//
// This was survivable on the prototype because stdio there was USB CDC,
// which discards output when no host is attached. UART0 does not: the FIFO
// drains at the wire rate whether or not anyone is listening, so the cost
// is now paid on every boot of every board.
//
// Same shape as DEBUG_I8237 and friends in chipset/, so there is one habit
// to learn rather than four.
//#define DEBUG_MC6845
#if defined(DEBUG_MC6845)
#define crtc_log(...) printf(__VA_ARGS__)
#else
#define crtc_log(...)
#endif
#include "i8237.h"
#include "i8253.h"
#include "i8259.h"
#include "i8272.h"
#include "ide.h"
#include "uart16550.h"
#include "mc146818.h"
#include "adlib.h"
#include "audio.h"
#include "redirector.h"
#include "graphics.h"
#include "hardware/pwm.h"
// ============================================================================
// External Port Arrays and Variables
// ============================================================================
extern uint8_t current_scancode; // Keyboard scancode (defined in main.c)
// ============================================================================
// Port State Variables
// ============================================================================
uint8_t port3DA = 0; // VGA status port state
uint8_t port61 = 0;  // System Control Port B (8255 PPI Port B)

extern cga_s cga;
extern mc6845_s mc6845;

static uint8_t crtc_index = 0;
static uint8_t tga_index = 0;

__force_inline static uint8_t port_read8(const uint32_t address) {
    // if (address >= 0x300)
    // printf("port read %03x\n", address);
    switch (address) {
        case 0x3D4:
            return crtc_index;
        case 0x3D5:
            return mc6845.registers[crtc_index];
        case 0x3D8:
            return cga.port3D8;
        case 0x3D9:
            return cga.port3D9;
        case 0x3DA: // MC6845 status port
            return port3DA;
        case 0 ... 0x0F: {
            return i8237_readport(address);
        }

        case 0x20 ... 0x21: {
            return i8259_read(address);
        }
        case 0x40 ... 0x42: {
            return i8253_read(address);
        }
        case 0x60:
            return current_scancode;
        case 0x61:
            return port61;
        case 0x62: {
            uint8_t r = 0;
            if (port61 & 0x8) {
                r |= 1 << 2; // 2 FDD
                // r |= 0b01; // CGA 40x25
                r |= 0b10; // CGA 80x25
                //r |= 0b11; // MdA
            } else {
                r |= 0x4;
            }
            return r;
        }
        case 0x64: {
            // Keyboard Controller Status Register (Intel 8042)
            // Bit 0: Output buffer full (1 = данные доступны в 0x60)
            // Bit 1: Input buffer full (0 = готов принять команду)
            // Bit 2: System flag (1 = POST passed) - КРИТИЧНО для DOS!
            // Bit 3: Command/Data (0 = data, 1 = command)
            // Bit 4: Keyboard enabled
            // Bit 5-7: таймауты и ошибки (0)

            uint8_t status = 0x14; // Биты 2 (System) и 4 (Keyboard enabled) установлены

            // Bit 0: есть данные для чтения
            if (current_scancode != 0) {
                status |= 0x01;
            }

            return status;
        }
        case 0x71: {
            return cmos_read_data();
        }
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x87: {
            return i8237_readpage(address);
        }
        case 0x300 ... 0x308: {
            return ide_read(address);
        }
        case 0x388: {
            return adlib_status();
        }
        case 0x0E0: case 0x0E1: case 0x0E2: {
            return (uint8_t)redirector_read(address, false);
        }
        case 0x3F4: case 0x3F5: {
            return i8272_readport(address);
        }
        case 0x3F8 ... 0x3FF: {
            // COM1 (Intel 16550 UART)
            return uart_read(address);
        }
        default:
            bus_trace(0);
            return 0xFF;
    }
}

__force_inline static void port_write8(const uint32_t address, const uint8_t data, const bool bhe) {
    switch (address) {
        case 0 ... 0x0F: {
            return i8237_writeport(address, data);
        }
        case 0x20 ... 0x21: {
            return i8259_write(address, data);
        }
        case 0x40 ... 0x43: {
            return i8253_write(address, data);
        }
        case 0x60: {
            current_scancode = 0xAA;
            return;
        }
        case 0x61: {
            // System Control Port B (8255 PPI Port B). Bits 0 and 1
            // together gate the speaker: bit 0 is the PIT channel-2 gate,
            // bit 1 the driver enable.
            //
            // Recorded, not acted on. GP46 is a PWM DAC now and the square
            // wave is synthesised on core 0; this runs in the bus
            // interrupt and has no business touching a peripheral.
            audio_speaker_gate((data & 3) == 3);

            // Бит 7: Clear keyboard (IBM PC XT standard)
            if ((data & 0x80) && !(port61 & 0x80)) {
                current_scancode = 0;
            }

            if ((data & 0x40) && !(port61 & 0x40)) {
                current_scancode = 0xAA;
                i8259_interrupt(1);
            }

            port61 = data;
            return;
        }

        case 0x70: {
            cmos_write_index(data);
            return;
        }
        case 0x71: {
            cmos_write_data(data);
            return;
        }
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x87: {
            return i8237_writepage(address, data);
        }
        case 0x300 ... 0x308: {
            return ide_write(address, data);
        }
        case 0x388: case 0x389: {
            adlib_write(address, data);
            return;
        }
        case 0x0E0: case 0x0E1: case 0x0E2: {
            redirector_write(address, data, false);
            return;
        }

        case 0x3B0:
        case 0x3B2:
        case 0x3B4:
        case 0x3B6:
        case 0x3D0:
        case 0x3D2:
        case 0x3D4:
        case 0x3D6:
            crtc_index = data;
            return;
        case 0x3B1:
        case 0x3B3:
        case 0x3B5:
        case 0x3B7:
        case 0x3D1:
        case 0x3D3:
        case 0x3D5:
        case 0x3D7:
            mc6845.registers[crtc_index] = data;

            if (crtc_index < 0xc)
                crtc_log("MC6845 register %x (%d) : %x (%d) \n", crtc_index, crtc_index, data, data);

            // TODO: Сразу вычислять поинтер  VIDEORAM чтобы в потребителе не тратить времея на вычисления
            mc6845.vram_offset = (mc6845.r.start_addr_h << 8 | mc6845.r.start_addr_l) << 1;

            const uint16_t cursor_offset = (mc6845.r.cursor_addr_h << 8 | mc6845.r.cursor_addr_l);
            mc6845.cursor_x = cursor_offset % mc6845.r.h_displayed;
            mc6845.cursor_y = cursor_offset / mc6845.r.h_displayed;
            return;
        case 0x3B8:
        case 0x3D8: {
            cga.port3D8 = data; // Store the raw register value
            mc6845.text_blinking_mask = (data & 0b100000) ? 0x7F : 0xFF; // BIT 5: blinking enabled (маска для бита атрибута)
            cga.updated = true;
            return;
        }
        case 0x3BF:
        case 0x3D9:
            cga.port3D9 = data;
            cga.updated = true;
            return;

        case 0x3DA: {
            static bool tga_flip_flop = false;

            if (tga_flip_flop == false) {
                tga_index = data & 0x1F;
            } else {
                if (tga_index == 3) {
                    cga.port3DA_tandy = data;
                    cga.updated = true;
                }
                // else if (tga_index & 0x10) {
                // graphics_set_palette(tga_index & 0xF, cga_palette[data & 0xF]);
                // }
                // printf("3DA: Tandy video write %x %x\n", tga_index, data);
            }
            tga_flip_flop ^= 1;
            return;
        }
         case 0x3DE: {
            cga.port3DA_tandy = data;
            cga.updated = true;
            // Why Prince Writes here instead of 3da???
            // printf("3DE: Tandy video write %x %x\n", tga_index, data);
        }
        case 0x3F2: case 0x3F5: {
            return i8272_writeport(address, data);
        }
        case 0x3F8 ... 0x3FF: {
            // COM1 (Intel 16550 UART)
            return uart_write(address, data);
        }
            default:
            bus_trace(0);
    }
}

// ============================================================================
// Port Read (16-bit)
// ============================================================================
__force_inline static uint16_t port_read(const uint32_t address, const bool bhe) {
    // Оптимизация: проверяем A0 и BHE для выбора 8/16-битного пути
    const uint8_t a0 = (address & 1) << 3;

    const uint8_t byte = port_read8(address);

    // BHE=0, A0=0 -> 16-битная операция word (оба байта)
    if (unlikely(!bhe && !a0)) {
        return byte | (port_read8(address + bhe) << 8);
    }

    return byte << a0;

    // BHE=1, A0=1 -> невалидная комбинация (не используется в i8086)
    // return 0xFFFF;A
}

// ============================================================================
// Port Write (16-bit with BHE support)
// ============================================================================
__force_inline static void port_write(const uint32_t address, const uint16_t data, const bool bhe) {
    // Оптимизация: проверяем A0 и BHE для выбора 8/16-битного пути
    const uint8_t a0 = (address & 1) << 3;

    port_write8(address, data >> a0, bhe);

    // BHE=0, A0=0 -> 16-битная операция word (оба байта)
    if (!bhe && !a0) {
        // port_write8(address, data, bhe);
        port_write8(address + 1, data >> 8, bhe);
    }

    // BHE=1, A0=1 -> невалидная комбинация (игнорируем)
}
