/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "adlib.h"

#include <stdio.h>

#include "emu8950.h"

static OPL     *opl;
static uint8_t  reg_index;      // last write to 388h
static uint8_t  timer_reg;      // register 4, the only one status depends on
static bool     touched;        // has the guest ever written a voice register?

void adlib_init(const uint32_t sample_rate_hz) {
    opl = OPL_new(OPL2_CLOCK_HZ, sample_rate_hz);
    if (opl) OPL_reset(opl);
    reg_index = 0;
    timer_reg = 0;
    touched   = false;
    printf("[opl2] YM3812 %s at %lu Hz\n",
           opl ? "ready" : "FAILED to allocate", (unsigned long)sample_rate_hz);
}

bool adlib_present(void) { return opl != NULL; }

void adlib_write(const uint16_t port, const uint8_t value) {
    if (port & 1) {
        // 389h — data. Register 4 is the timer control, and status is
        // derived from it, so it is kept even when there is no chip.
        if (reg_index == 0x04) {
            timer_reg = value;
            // Bit 7 is RESET-IRQ: it clears the flags and is not stored.
            if (value & 0x80) timer_reg = 0;
        } else {
            touched = true;
        }
        if (opl) OPL_writeReg(opl, reg_index, value);
    } else {
        reg_index = value;   // 388h — register select
    }
}

uint8_t adlib_status(void) {
    // Reproduced from the timer register rather than from a running
    // timer. Detection only ever asks "did the flags change after I
    // started a timer", and a card that answers 0x00 before and 0xC0
    // after satisfies every routine that asks.
    if (!timer_reg) return 0x00;
    return (uint8_t)(0x80
                     + (timer_reg & 0x01) * 0x40      // timer 1 expired
                     + (timer_reg & 0x02) * 0x10);    // timer 2 expired
}

bool adlib_render(int32_t *out, const uint32_t nsamples) {
    if (!opl || !touched) return false;
    OPL_calc_buffer_linear(opl, out, nsamples);
    return true;
}
