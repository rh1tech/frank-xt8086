/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * MC146818 CMOS RTC at ports 70h/71h, backed by the DS3231 (U15).
 *
 * A real XT had no such thing — the AT introduced it — but every RTC
 * driver written since knows this interface, so it is the one worth
 * presenting. GLaBIOS does not read it itself; it reserves a BDA word for
 * GLaTICK, its RTC option ROM, and a DOS TSR can read it directly.
 *
 * Two rules govern everything here:
 *
 * 1. No I2C on this path. port_read()/port_write() run inside the PIO bus
 *    interrupt at the highest priority with the 8086 stalled in a wait
 *    state. A DS3231 transaction is on the order of a hundred
 *    microseconds. So the clock registers are served from a snapshot that
 *    core 0 refreshes once a second, and a guest write is recorded and
 *    flushed by core 0 later. cmos_tick() is that pump.
 *
 * 2. The register file is the truth for everything except the ten clock
 *    registers. CMOS RAM above 0Dh is ordinary storage the guest owns.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ds3231.h"

// Register numbers, as every BIOS since 1984 numbers them.
#define CMOS_SECONDS      0x00
#define CMOS_SECONDS_ALRM 0x01
#define CMOS_MINUTES      0x02
#define CMOS_MINUTES_ALRM 0x03
#define CMOS_HOURS        0x04
#define CMOS_HOURS_ALRM   0x05
#define CMOS_DAY_OF_WEEK  0x06
#define CMOS_DAY_OF_MONTH 0x07
#define CMOS_MONTH        0x08
#define CMOS_YEAR         0x09
#define CMOS_STATUS_A     0x0A
#define CMOS_STATUS_B     0x0B
#define CMOS_STATUS_C     0x0C
#define CMOS_STATUS_D     0x0D
#define CMOS_CENTURY      0x32   // by convention, not by the datasheet

#define CMOS_RAM_SIZE     0x40   // 64 registers, the original part's size

// Status A
#define CMOS_A_UIP        0x80   // update in progress
// Status B
#define CMOS_B_24HOUR     0x02
#define CMOS_B_BINARY     0x04   // clear = BCD, which is what BIOSes assume
#define CMOS_B_SET        0x80   // guest is mid-update; do not advance
// Status D
#define CMOS_D_VRT        0x80   // "valid RAM and time" — clear means dead battery

typedef struct {
    uint8_t    reg[CMOS_RAM_SIZE];  // the whole register file
    uint8_t    index;               // last value written to port 70h
    bool       nmi_disabled;        // bit 7 of that write
    rtc_time_t snapshot;            // clock, refreshed by core 0
    bool       valid;               // is the snapshot real time?
    bool       dirty;               // guest changed the clock; flush it

    // Free-running fallback, used when the DS3231 cannot be trusted: the
    // seed and the RP2350 microsecond count when it was taken. The clock
    // then advances off the system timer, so a board with a flat coin
    // cell still keeps time within a session.
    uint32_t   seed_epoch;
    uint64_t   seed_us;
} mc146818_s;

extern mc146818_s cmos;

/*
 * Bring the model up: probe the DS3231, take a first snapshot, and set
 * the status registers to what a healthy AT would show.
 *
 * Called from core 0 before core 1 releases the CPU, so it may use I2C.
 */
void cmos_init(void);

/*
 * Pump: refresh the snapshot, and flush a guest-set time to the DS3231.
 *
 * Must be called from core 0 only, and never from the bus handler. Cheap
 * to call often — it does I2C at most once a second, and only when the
 * guest has actually changed something.
 */
void cmos_tick(void);

// ---------------------------------------------------------------------------
// Guest-facing ports
// ---------------------------------------------------------------------------
//
// These two run in the bus interrupt. Everything they touch is memory.

static inline void cmos_write_index(const uint8_t v) {
    // Bit 7 is the NMI mask, not part of the register number. Masking it
    // off is what stops a BIOS that disables NMI while poking the CMOS
    // from addressing register 0x8n and falling off the register file.
    cmos.index        = v & 0x7Fu;
    cmos.nmi_disabled = (v & 0x80u) != 0;
}

uint8_t cmos_read_data(void);
void    cmos_write_data(uint8_t v);
