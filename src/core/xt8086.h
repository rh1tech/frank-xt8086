/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Board facts: what is wired where, how fast things run, how much memory
 * there is. Nothing here does anything — it is the one file to open when
 * the question is "which pin is that".
 *
 * The numbers are read off xt8086_beta.kicad_pcb, not inherited. GP0-GP29
 * are the 8086's own bus and their assignment is fixed by the CPU package;
 * everything above GP29 is a choice the board made, and the choices are
 * different from the WeAct module the prototype ran on. See boards/
 * frank_xt8086.h for the SDK-facing half of the same map.
 */
#pragma once

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <hardware/vreg.h>
#include <hardware/clocks.h>
#include <hardware/sync.h>
#include <hardware/pio.h>
#include <pico/time.h>

// ============================================================================
// Compiler hints for branch prediction
// ============================================================================
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

// ============================================================================
// Clocks
// ============================================================================
//
// CPU_SPEED_MHZ comes from CMake so a board that will not hold the fast
// clock can be dropped to a safe one without editing a header. 504 MHz is
// what the prototype ran at and what the VGA timings were measured
// against; 252 MHz is the conservative fallback, and the PIO dividers all
// derive from clk_sys so both are correct, just differently sharp.
#ifndef CPU_SPEED_MHZ
#define CPU_SPEED_MHZ        504
#endif
#define PICO_CLOCK_SPEED     (CPU_SPEED_MHZ * MHZ)

// Ceiling for the QSPI clock feeding the PSRAM. The QMI divides clk_sys
// down to at most this; see src/drivers/psram.
#ifndef PSRAM_SPEED_MHZ
#define PSRAM_SPEED_MHZ      166
#endif
#define PSRAM_FREQ_HZ        (PSRAM_SPEED_MHZ * MHZ)

#define I8086_CLOCK_SPEED    (6000 * KHZ)  // i8086 clock frequency
#define I8086_DUTY_CYCLE     (33)          // 33% duty cycle required for i8086

// ============================================================================
// Memory map
// ============================================================================
#define VIDEORAM_SIZE        (32 * 1024)   // Tandy/PC Jr compat
#define RAM_SIZE             (736 * 1024)
#define UMB_SIZE             (128 * 1024)

#define BIOS_ROM_SIZE        (8 * 1024)                         // 8KB BIOS
#define BIOS_ROM_BASE        (0x100000 - BIOS_ROM_SIZE)         // 0xFE000-0xFFFFF

// ============================================================================
// GPIO pin map
// ============================================================================
//
// GP0-GP29 belong to the 8086 (U5) and are not negotiable: AD0-AD15 on
// GP0-GP15, A16-A19 on GP16-GP19, then the control signals in the order
// the PIO program scans them. src/core/i8086_bus.pio repeats these as .define
// constants because a PIO program cannot include a C header; the two lists
// must agree, and the PIO file says so at the point it declares them.
#define ALE_PIN          20             // Address Latch Enable, from the CPU
#define RD_PIN           21             // Read strobe   (active LOW)
#define WR_PIN           22             // Write strobe  (active LOW)
#define INTA_PIN         23             // Interrupt acknowledge (active LOW)
#define MIO_PIN          24             // Memory / IO select (1 = memory)
#define BHE_PIN          25             // Bus High Enable
#define INTR_PIN         26             // Interrupt request out (active HIGH)
#define READY_PIN        27             // READY, through the U2 flip-flop
#define RESET_PIN        28             // Reset out (active HIGH)
#define CLOCK_PIN        29             // Clock out to the CPU

// Video: a six-bit R2R ladder plus separate sync, GP30-GP37. The byte the
// PIO shifts out is VHRRGGBB — blue in the low pair, which is what R27/R28
// (800R/400R into the blue node) make it. Separate H and V, so no CSYNC.
#define VGA_BASE_PIN     30

// DS3231 (U15), bit-banged. 4.7k pull-ups are on the board.
#define I2C_SDA_PIN      38
#define I2C_SCL_PIN      39

// microSD (J4) on SPI1 — GP40-43 have no SPI0 mapping at all.
#define SDCARD_MISO_PIN  40
#define SDCARD_CS_PIN    41
#define SDCARD_SCK_PIN   42
#define SDCARD_MOSI_PIN  43

// Console UART (J1: pin 1 TX, pin 3 RX). GP44/45 land back on UART0.
#define UART_TX_PIN      44
#define UART_RX_PIN      45

#define BEEPER_PIN       46             // PC speaker / HDMI audio / jack
#define PSRAM_CS_PIN     47             // ESP-PSRAM64H (U8) on XIP CS1

// ---------------------------------------------------------------------------
// Optional: a scope probe that goes low on an access to unmapped address
// space, for catching a BIOS that is reading a card this board does not
// have. The prototype hard-wired this to GP45 and always drove it; here
// GP45 is the console UART's RX, so driving it would fight the FTDI cable
// on the one channel that reports what went wrong. Off unless a spare pin
// is named at configure time (-DBUS_TRACE_PIN=n).
#ifdef BUS_TRACE_PIN
#define bus_trace(level)  gpio_put(BUS_TRACE_PIN, (level))
#else
#define bus_trace(level)  ((void)0)
#endif

// ============================================================================
// Bus state bits
// ============================================================================
//
// The PIO pushes GPIO 0-25 as one word, so the control signals arrive in
// the same register as the address and are tested in place.
#define MIO (1 << MIO_PIN)  // Memory/IO bit in bus state
#define BHE (1 << BHE_PIN)  // Bus High Enable bit in bus state

// ============================================================================
// PIO Configuration
// ============================================================================
#define BUS_CTRL_PIO     pio0
#define BUS_CTRL_SM      0
#define WRITE_IRQ        PIO0_IRQ_0
#define READ_IRQ         PIO0_IRQ_1

#define IMPORT_BIN(file, sym) asm (\
".section .flashdata."#sym"\n"                  /* Change section */\
".balign 4\n"                           /* Word alignment */\
".global " #sym "\n"                    /* Export the object address */\
#sym ":\n"                              /* Define the object label */\
".incbin \"" file "\"\n"                /* Import the file */\
".global _sizeof_" #sym "\n"            /* Export the object size */\
".set _sizeof_" #sym ", . - " #sym "\n" /* Define the object size */\
".balign 4\n"                           /* Word alignment */\
".section \".text\"\n");                 /* Restore section */

// ============================================================================
// Core entry points
// ============================================================================
void cpu_bus_init(void);
void start_cpu_clock(uint32_t frequency_khz);
void reset_cpu(void);
void mc6845_init_text_mode(void);  // MC6845 defaults for 80x25 text
