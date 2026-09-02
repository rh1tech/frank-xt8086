/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * One console byte -> one XT scancode.
 *
 * SETUP grew this first, because a board on the bench is often driven
 * over J1 with no USB keyboard attached. The drive menu needed exactly
 * the same translation and did not have it -- it read USB alone, so the
 * one menu you reach *while the guest is running* was the one menu the
 * console could not touch. That made a fault an operator could hit
 * trivially something nobody could reproduce from a terminal.
 *
 * Independent of SERIAL_CONSOLE: that option decides whether the guest
 * sees the console as a keyboard, which is a different question from
 * whether the firmware's own menus do.
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <pico/error.h>
#include <pico/stdio.h>

// How long to wait for the rest of an escape sequence. Long enough for
// one to arrive over a 115200 line, short enough not to feel like a hang.
#define CONKEY_SEQ_US 50000u

/*
 * Returns an XT scancode, or 0 if nothing arrived within poll_us.
 *
 * Only the keys the menus actually act on are translated. Anything else
 * is returned as its raw byte, which the callers ignore.
 */
static inline uint8_t console_scancode(const uint32_t poll_us) {
    int ch = getchar_timeout_us(poll_us);
    if (ch == PICO_ERROR_TIMEOUT) return 0;

    if (ch == '\r' || ch == '\n') return 0x1C;   // ENTER
    if (ch == 0x7F) return 0x53;                 // DEL, as most terminals send it

    if (ch != 0x1B) return (uint8_t)ch;

    // Bare ESC, or the start of a cursor-key sequence.
    ch = getchar_timeout_us(CONKEY_SEQ_US);
    if (ch == PICO_ERROR_TIMEOUT) return 0x01;   // ESC

    if (ch != '[' && ch != 'O') return 0x01;

    const int c2 = getchar_timeout_us(CONKEY_SEQ_US);
    switch (c2) {
        case 'A': return 0x48;   // UP
        case 'B': return 0x50;   // DOWN
        case 'C': return 0x4D;   // RIGHT
        case 'D': return 0x4B;   // LEFT
        default: break;
    }

    // ESC [ 3 ~ is Delete. Consume the tilde so it is not read as a key
    // of its own on the next call.
    if (c2 == '3') {
        (void)getchar_timeout_us(CONKEY_SEQ_US);
        return 0x53;
    }
    return 0;
}
