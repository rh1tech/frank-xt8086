/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>

typedef struct {
    uint8_t misc;
    uint8_t seq[5];
    uint8_t crtc[25];
    uint8_t gc[9];
    uint8_t ac[21];
} VgaRegs;

typedef struct {
    uint8_t  mode;
    uint8_t  text;
    uint16_t cols;
    uint8_t  rows_minus_1;
    uint8_t  char_height;
    uint16_t page_size;
    uint16_t crtc_base;
    uint32_t clear_base;
    uint32_t clear_size;
    const VgaRegs *regs;
} VgaMode;

const VgaMode *vgamodes_find(uint8_t mode);
