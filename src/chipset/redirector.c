/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "redirector.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "state.h"

// ---------------------------------------------------------------------------
// The wire
// ---------------------------------------------------------------------------

#define FW_CMD_BEGIN 0
#define FW_CMD_EXEC  1

static guest_regs_t regs;
static uint8_t      slot;        // which word of the register file is next
static volatile bool pending;    // a request is waiting for core 0
static volatile bool done;       // ...and core 0 has finished it

/*
 * Three answers, not two.
 *
 * Carry clear is success and carry set is a DOS error, but there is a
 * third case: a call we do not implement. That must be *declined* so the
 * TSR passes it down the chain rather than failing it -- another
 * redirector may be below us, and a call we wrongly claim and botch
 * corrupts data where a declined one merely moves on.
 */
#define ANSWER_OK      0
#define ANSWER_ERROR   1
#define ANSWER_DECLINE 2

static uint8_t answer = ANSWER_DECLINE;

// The register file as an array, so the port handler can index it rather
// than switch on ten cases. The order is the stub's order and the two
// must not drift.
static uint16_t *reg_slot(const uint8_t n) {
    switch (n) {
        case 0: return &regs.ax; case 1: return &regs.bx;
        case 2: return &regs.cx; case 3: return &regs.dx;
        case 4: return &regs.si; case 5: return &regs.di;
        case 6: return &regs.ds; case 7: return &regs.es;
        case 8: return &regs.ss; case 9: return &regs.sp;
        default: return NULL;
    }
}

/*
 * Word I/O arrives here as two byte accesses.
 *
 * port_write() splits a 16-bit OUT into port and port+1, which is what
 * the 8086 bus actually does -- so the stub's `out dx, ax` shows up as a
 * write to 0E0h and one to 0E1h. The low half is staged and the high half
 * commits the pair, which also means a stray byte-sized access cannot
 * advance the register index on its own.
 */
static uint16_t staging;

void redirector_write(const uint16_t port, const uint16_t value, const bool word) {
    (void)word;
    const uint8_t v = (uint8_t)value;

    switch (port) {
        case FW_PORT_CTRL:
            if (v == FW_CMD_BEGIN) {
                slot = 0;
                done = false;
            } else if (v == FW_CMD_EXEC) {
                slot = 0;          // rewind, ready to hand results back
                pending = true;    // core 0 takes it from here
            }
            return;

        case FW_PORT_DATA:              // low half
            staging = (staging & 0xFF00u) | v;
            return;

        case FW_PORT_DATA + 1: {        // high half: commit
            staging = (uint16_t)((staging & 0x00FFu) | ((uint16_t)v << 8));
            uint16_t *r = reg_slot(slot);
            if (r) { *r = staging; slot++; }
            return;
        }
        default: return;
    }
}

uint16_t redirector_read(const uint16_t port, const bool word) {
    (void)word;

    if (port == FW_PORT_CTRL) return done ? 1u : 0u;

    // Six registers come back, then the carry flag as a seventh word.
    if (port == FW_PORT_DATA) {
        if (slot < 6) {
            const uint16_t *r = reg_slot(slot);
            staging = r ? *r : 0;
        } else if (slot == 6) {
            staging = answer;
        } else {
            staging = 0xFFFFu;
        }
        return staging & 0xFFu;
    }

    if (port == FW_PORT_DATA + 1) {
        const uint16_t hi = (staging >> 8) & 0xFFu;
        slot++;                          // the pair is complete
        return hi;
    }
    return 0xFFu;
}

// ---------------------------------------------------------------------------
// DOS
// ---------------------------------------------------------------------------

// The error codes DOS expects back in AX when carry is set.
#define DOS_OK              0x00
#define DOS_FILE_NOT_FOUND  0x02
#define DOS_PATH_NOT_FOUND  0x03
#define DOS_ACCESS_DENIED   0x05
#define DOS_INVALID_DRIVE   0x0F
#define DOS_NO_MORE_FILES   0x12

static void fail(const uint16_t code) { regs.ax = code; answer = ANSWER_ERROR; }
static void ok(void)                  { answer = ANSWER_OK; }
static void decline(void)             { answer = ANSWER_DECLINE; }

/*
 * Read a byte out of the guest's memory.
 *
 * The whole reason this design works: RAM[] *is* the 8086's memory, so a
 * far pointer out of its registers is an array index here. No copying, no
 * window, no marshalling of buffers -- only the register file had to come
 * across the wire.
 */
static uint8_t guest_peek(const uint16_t seg, const uint16_t off) {
    const uint32_t pa = ((uint32_t)seg << 4) + off;
    return pa < RAM_SIZE ? RAM[pa] : 0xFF;
}

static uint8_t guest_peek_lin(const uint32_t pa) {
    return pa < RAM_SIZE ? RAM[pa] : 0xFF;
}

static void guest_poke(const uint16_t seg, const uint16_t off, const uint8_t v) {
    const uint32_t pa = ((uint32_t)seg << 4) + off;
    if (pa < RAM_SIZE) RAM[pa] = v;
}

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

/*
 * AX=110Ch — get disk information, which is what DOS asks before it will
 * believe the drive exists.
 *
 * The numbers are a polite fiction: a card of any size is reported as a
 * 32 MB volume with plenty free, because DOS's fields are 16-bit and the
 * honest answer does not fit. Every emulator does this, and DIR only ever
 * prints the total.
 */
static void fn_disk_info(void) {
    regs.ax = 0x0200;   // sectors per cluster
    regs.bx = 0x0400;   // total clusters
    regs.cx = 0x0200;   // bytes per sector
    regs.dx = 0x0300;   // available clusters
    ok();
}

/*
 * Where the Swappable Data Area lives in guest memory.
 *
 * DOS hands it over once, in BX:DX on the installation check, and every
 * other function needs it: the filename being operated on, the caller's
 * DTA and the search block all live inside it. Without it none of the
 * file operations can be implemented at all.
 */
static uint32_t sda_addr;

static void fn_install_check(void) {
    if (regs.bx || regs.dx) {
        sda_addr = ((uint32_t)regs.bx << 4) + regs.dx;
        printf("[redir] SDA at %04X:%04X (linear %05lX)\n",
               regs.bx, regs.dx, (unsigned long)sda_addr);
    }
    // AL non-zero means "a redirector is here". 0xFF is the conventional
    // answer; DOS only tests for non-zero.
    regs.ax = (regs.ax & 0xFF00u) | 0xFFu;
    ok();
}

void redirector_task(void) {
    if (!pending) return;
    pending = false;

    const uint8_t fn = regs.ax & 0xFFu;
    answer = ANSWER_DECLINE;

    printf("[redir] AX=%04X BX=%04X CX=%04X DX=%04X\n",
           regs.ax, regs.bx, regs.cx, regs.dx);

    switch (fn) {
        case 0x00: fn_install_check(); break;
        case 0x0C: fn_disk_info();     break;

        default:
            /*
             * Everything else is declined, deliberately and visibly.
             *
             * A redirector that answers some calls and silently mishandles
             * others corrupts data; one that says "not supported" makes
             * DOS report an error the operator can see. Until each
             * function is written and tested, that is the honest answer.
             */
            // Declined, and said out loud once per function so the gap is
            // visible rather than inferred from DOS's behaviour.
            printf("[redir] AX=%04X declined (not implemented)\n", regs.ax);
            decline();
            break;
    }

    done = true;
}
