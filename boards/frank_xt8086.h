// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------
//
// xt8086: an RP2350B acting as the whole chipset for a real 8086.
//
// It gets a header of its own rather than sharing frank_b.h, because the
// two things a board header actually decides — where the console is and
// where the default SPI is — are both different here, and both would be
// wrong in a way that matters. frank_b.h puts the console on GP0/GP1;
// on this board those are the CPU's AD0 and AD1, so a shared header
// would have every printf drive two lines of a live address bus. The
// microSD is on SPI1 at GP40-43 rather than SPI0 at GP4-7 for the same
// underlying reason: everything below GP30 belongs to the 8086.
//
// The package, the flash part and the flash size are the same as
// frank_b.h's, and that is the whole of what these two headers agree on.
//
// Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
// https://github.com/rh1tech/frank-test
// SPDX-License-Identifier: GPL-3.0-or-later
//
#ifndef _BOARDS_FRANK_XT8086_H
#define _BOARDS_FRANK_XT8086_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define FRANK_XT8086

// --- RP2350 VARIANT ---
// RP2350B: 48 GPIOs, so PIO needs the movable 32-pin window — and this
// board needs it more than most, since its video sits on GP30-37 and a
// state machine left in the default window cannot see past GP31.
#define PICO_RP2350A 0

pico_board_cmake_set_default(PICO_PIO_USE_GPIO_BASE, 1)
#ifndef PICO_PIO_USE_GPIO_BASE
#define PICO_PIO_USE_GPIO_BASE 1
#endif

// --- UART (J1: pin 1 = TX, pin 3 = RX) ---
// GP44/GP45, which is still UART0: the peripheral's pin map repeats
// every sixteen GPIOs in blocks of four, and GP44-47 lands back on
// UART0 just as GP0-3 does.
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 44
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 45
#endif

// --- LED ---
// There is no firmware LED. LD1 hangs off GP25, which is the 8086's
// ~BHE — the CPU lights it, and declaring it here would invite SDK
// helpers to drive a pin the CPU also drives. LD2 is wired to 3V3 and
// only says the board has power.

// --- SPI (microSD, J4) ---
// SPI1, because GP40-43 have no SPI0 mapping at all.
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 1
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 42
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 43
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 40
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 41
#endif

// --- I2C (DS3231, U15) ---
// I2C1 on GP38/GP39. The firmware bit-bangs this bus rather than using
// the peripheral, but the mapping is declared so the numbers are in one
// place with the rest.
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 1
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 38
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 39
#endif

// --- FLASH (U7, W25Q128JVPIQ, 16 MB) ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
