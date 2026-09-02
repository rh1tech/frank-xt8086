/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * AdLib / Yamaha YM3812 (OPL2) at ports 388h/389h.
 *
 * The register file and the synthesis are emu8950's, vendored in
 * drivers/opl2. This file is only the bus face of it: the two ports, the
 * status register games use to detect the card, and the rule that keeps
 * synthesis off the interrupt path.
 *
 * That rule is the same one the CMOS RTC follows. port_write() runs in
 * the PIO bus interrupt with the 8086 stalled in a wait state; rendering
 * even one OPL sample there would be far too slow. So a write is recorded
 * into the chip's register file — which is cheap, it is a table write —
 * and core 0 does all the sample generation in adlib_render().
 *
 * Detection, for reference, is the sequence every AdLib-aware game runs:
 * reset both timers (reg 4 <- 0x60, then 0x80), read status, start timer 1
 * (reg 2 <- 0xFF, reg 4 <- 0x21), wait ~80 us, read status again. A real
 * card returns 0x00 then 0xC0. adlib_status() reproduces that from the
 * timer register alone, without running a timer, which is what pico-286
 * does and what every emulator has done since DOSBox.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// The YM3812's master clock on an AdLib card: 3.579545 MHz, the NTSC
// colour burst, because that crystal was the cheapest part in 1987.
#define OPL2_CLOCK_HZ 3579545u

/*
 * Bring the synthesiser up. Allocates, so it must run before core 1 does
 * and never from the bus handler.
 *
 * Safe to call when audio is disabled; the port handlers then just keep a
 * register file nobody renders, which is all a detection routine needs.
 */
void adlib_init(uint32_t sample_rate_hz);

// Was the synthesiser created?
bool adlib_present(void);

// ---------------------------------------------------------------------------
// Guest-facing ports — these run in the bus interrupt
// ---------------------------------------------------------------------------

void    adlib_write(uint16_t port, uint8_t value);
uint8_t adlib_status(void);

// ---------------------------------------------------------------------------
// Core 0
// ---------------------------------------------------------------------------

/*
 * Render `nsamples` mono samples into `out`.
 *
 * 32-bit, because EMU8950_LINEAR selects OPL_calc_buffer_linear() and
 * that is the width it produces -- and the mixer sums in 32 bits anyway,
 * so taking it in the native width saves a copy and a clip.
 *
 * Returns false when there is nothing to render — no chip, or the guest
 * has never touched the card — so the caller can skip mixing entirely
 * and a machine that never plays a note costs nothing.
 */
bool adlib_render(int32_t *out, uint32_t nsamples);
