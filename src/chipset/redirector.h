/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The DOS network redirector: drive H: is the microSD's filesystem.
 *
 * DOS's INT 2Fh AH=11h interface, answered from FatFs. MAPDRIVE.COM
 * flags H: in the current-directory structure as a network drive; from
 * then on every access to it arrives here.
 *
 * Why this exists at all, rather than being ported from pico-286: that is
 * a software CPU emulator and it services the redirector by trapping
 * INT 2Fh inside its instruction decoder, reading and writing its own
 * CPU_AX and friends. There is no decode step here and no register file
 * to reach into -- the 8086 is a real chip. So the option ROM carries a
 * stub that hands the register file over through an I/O port, and this is
 * the other end of that wire. The filesystem logic below is pico-286's in
 * shape, but everything about how it is reached is different.
 *
 * The split follows the same rule as every other chip model here: the
 * port handlers run in the bus interrupt and do nothing but move bytes,
 * and all the FatFs work happens on core 0.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Two ports nothing on an XT uses. See tools/rom/xtrom.asm for the guest
// half of the protocol.
#define FW_PORT_DATA 0x0E0u
#define FW_PORT_CTRL 0x0E2u

// The register file, in the order the stub sends and receives it.
typedef struct {
    uint16_t ax, bx, cx, dx, si, di, ds, es, ss, sp;
    bool     carry;     // the redirector's success/failure channel
} guest_regs_t;

// ---------------------------------------------------------------------------
// Bus side — these run in the PIO interrupt
// ---------------------------------------------------------------------------

void    redirector_write(uint16_t port, uint16_t value, bool word);
uint16_t redirector_read(uint16_t port, bool word);

// ---------------------------------------------------------------------------
// Core 0
// ---------------------------------------------------------------------------

/*
 * Service a pending request, if there is one.
 *
 * Called from the main loop. The guest is spinning on the status port
 * while this runs, which is the same wait a physical disk would have
 * imposed, so taking milliseconds here is correct rather than merely
 * tolerable.
 */
void redirector_task(void);

// Which drive letter we answer for. H: is what MAPDRIVE.COM maps.
#define REDIR_DRIVE 'H'

// Where on the card the drive is rooted.
#define REDIR_ROOT  "/XT"
