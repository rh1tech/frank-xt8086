/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mc146818.h"

#include <stdio.h>
#include <string.h>

#include <pico/time.h>

mc146818_s cmos;

static uint8_t  to_bcd(const unsigned v) { return (uint8_t)(((v / 10u) << 4) | (v % 10u)); }
static unsigned from_bcd(const uint8_t v) { return (v >> 4) * 10u + (v & 0x0Fu); }

// Does the guest want binary or BCD? Every BIOS and DOS driver in
// practice leaves this at BCD, but honouring the bit costs one branch and
// a driver that sets it would otherwise read dates off by a factor of six.
static bool binary_mode(void) { return (cmos.reg[CMOS_STATUS_B] & CMOS_B_BINARY) != 0; }

static uint8_t enc(const unsigned v) {
    return binary_mode() ? (uint8_t)v : to_bcd(v);
}
static unsigned dec(const uint8_t v) {
    return binary_mode() ? v : from_bcd(v);
}

// ---------------------------------------------------------------------------
// Snapshot <-> register file
// ---------------------------------------------------------------------------

static void publish(const rtc_time_t *t) {
    cmos.reg[CMOS_SECONDS]      = enc(t->sec);
    cmos.reg[CMOS_MINUTES]      = enc(t->min);
    cmos.reg[CMOS_HOURS]        = enc(t->hour);
    cmos.reg[CMOS_DAY_OF_WEEK]  = enc(t->dow);
    cmos.reg[CMOS_DAY_OF_MONTH] = enc(t->day);
    cmos.reg[CMOS_MONTH]        = enc(t->mon);
    cmos.reg[CMOS_YEAR]         = enc(t->year % 100u);
    cmos.reg[CMOS_CENTURY]      = enc(t->year / 100u);
}

static void collect(rtc_time_t *t) {
    t->sec  = (uint8_t)dec(cmos.reg[CMOS_SECONDS]);
    t->min  = (uint8_t)dec(cmos.reg[CMOS_MINUTES]);
    t->hour = (uint8_t)dec(cmos.reg[CMOS_HOURS]);
    t->day  = (uint8_t)dec(cmos.reg[CMOS_DAY_OF_MONTH]);
    t->mon  = (uint8_t)dec(cmos.reg[CMOS_MONTH]);

    // The century register is a convention rather than a standard, so a
    // guest that never wrote it leaves whatever was there. Anything that
    // is not a plausible century is treated as 20xx, which is the only
    // century this hardware can be running in.
    const unsigned cent = dec(cmos.reg[CMOS_CENTURY]);
    t->year = (uint16_t)((cent >= 19u && cent <= 21u ? cent : 20u) * 100u
                         + dec(cmos.reg[CMOS_YEAR]));
    t->dow  = rtc_day_of_week(t->year, t->mon, t->day);
}

// ---------------------------------------------------------------------------
// Life cycle
// ---------------------------------------------------------------------------

void cmos_init(void) {
    memset(&cmos, 0, sizeof cmos);

    // Status A: divider 010 selects the 32.768 kHz time base, rate 0110
    // gives the 1024 Hz periodic rate an AT BIOS expects to find. We
    // generate no periodic interrupt, but reporting a nonsense divider
    // makes a driver think the oscillator is stopped.
    cmos.reg[CMOS_STATUS_A] = 0x26;
    cmos.reg[CMOS_STATUS_B] = CMOS_B_24HOUR;   // 24-hour, BCD
    cmos.reg[CMOS_STATUS_C] = 0x00;

    // Trusted means: a chip answered, and it has not lost power since
    // someone last set it. A DS3231 whose oscillator stopped is still
    // counting -- from whatever its registers happened to hold -- so
    // "the date parses" is not the same question as "the date is real".
    const bool have    = ds3231_init();
    const bool trusted = have && !ds3231_lost_power();

    rtc_time_t t;
    bool from_rtc = false;
    if (trusted && ds3231_read(&t) && rtc_time_valid(&t)) {
        from_rtc = true;
    } else {
        // No chip, a flat cell, or a date the calendar rejects. The build
        // time is a better answer than the RTC's 2000-01-01: it cannot be
        // right, but it is never absurd, and it never predates the
        // firmware that wrote the file.
        rtc_build_time(&t);
    }
    cmos.valid = from_rtc;

    cmos.snapshot   = t;
    cmos.seed_epoch = rtc_to_epoch(&t);
    cmos.seed_us    = time_us_64();
    publish(&t);

    // Status D bit 7 is how a BIOS asks "has this clock been set since it
    // last lost power". Answering honestly is what makes DOS prompt for
    // the date instead of silently believing a wrong one.
    cmos.reg[CMOS_STATUS_D] = cmos.valid ? CMOS_D_VRT : 0x00;

    printf("[rtc] DS3231 %s", have ? "found" : "absent");
    if (have && !trusted) {
        /*
         * "Oscillator stopped" on its own does not say which of the two
         * things went wrong, and they need different fixes: a clock that
         * has simply never been set reads back near its power-on default,
         * while one whose cell has gone flat reads back a time that was
         * plausible when the power went away. Printing what the chip
         * actually holds separates them without guessing.
         */
        rtc_time_t raw;
        if (ds3231_read(&raw)) {
            printf(", oscillator stopped; chip holds %04u-%02u-%02u %02u:%02u:%02u"
                   " (%s)",
                   raw.year, raw.mon, raw.day, raw.hour, raw.min, raw.sec,
                   raw.year <= 2001u ? "never set" : "set once, then lost power");
        } else {
            printf(", oscillator stopped and unreadable");
        }
    }
    printf(" -- %04u-%02u-%02u %02u:%02u:%02u (%s)\n",
           t.year, t.mon, t.day, t.hour, t.min, t.sec,
           from_rtc ? "from the RTC" : "from the build time, free-running");
}

void cmos_tick(void) {
    // A guest write wins over anything we might read back, so flush first.
    if (cmos.dirty) {
        cmos.dirty = false;

        rtc_time_t t;
        collect(&t);
        if (!rtc_time_valid(&t)) return;

        // Re-seed the fallback whatever happens: the guest has told us the
        // time, and that is worth keeping even if the chip refused it.
        cmos.snapshot   = t;
        cmos.seed_epoch = rtc_to_epoch(&t);
        cmos.seed_us    = time_us_64();

        if (ds3231_write(&t)) {
            cmos.valid    = true;
            cmos.reg[CMOS_STATUS_D] |= CMOS_D_VRT;
            printf("[rtc] guest set the clock to %04u-%02u-%02u %02u:%02u:%02u\n",
                   t.year, t.mon, t.day, t.hour, t.min, t.sec);
        }
        return;
    }

    // While the guest holds SET, its half-written registers are the truth
    // and overwriting them would corrupt the value it is assembling.
    if (cmos.reg[CMOS_STATUS_B] & CMOS_B_SET) return;

    // Once a second is enough for a clock whose finest unit is a second,
    // and it keeps the I2C bus almost entirely idle.
    static absolute_time_t next;
    if (absolute_time_diff_us(get_absolute_time(), next) > 0) return;
    next = make_timeout_time_ms(1000);

    rtc_time_t t;

    if (cmos.valid) {
        // The chip is the authority. Read it.
        if (!ds3231_read(&t) || !rtc_time_valid(&t)) return;
    } else {
        // It is not, so do not read it -- doing so would drag the guest's
        // clock back to the RTC's meaningless value once a second, and the
        // time would never advance at all. Run off the system timer
        // instead, from whatever the clock was seeded or last set to.
        const uint64_t elapsed = (time_us_64() - cmos.seed_us) / 1000000u;
        rtc_from_epoch(cmos.seed_epoch + (uint32_t)elapsed, &t);
    }

    cmos.snapshot = t;
    publish(&t);
}

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

uint8_t cmos_read_data(void) {
    const uint8_t idx = cmos.index;

    if (idx == CMOS_STATUS_C) {
        // Status C reports which interrupts fired and clears on read. We
        // raise none, so it is always zero — but it must still clear, or a
        // BIOS polling it after an IRQ8 that never comes spins forever.
        const uint8_t v = cmos.reg[CMOS_STATUS_C];
        cmos.reg[CMOS_STATUS_C] = 0;
        return v;
    }

    if (idx == CMOS_STATUS_A) {
        // UIP is set while the chip is copying its counters and the time
        // registers must not be read. We update them atomically from core
        // 0, so there is no such window and UIP is always clear — which is
        // also what a driver's "wait for UIP to fall" loop wants to see.
        return (uint8_t)(cmos.reg[CMOS_STATUS_A] & ~CMOS_A_UIP);
    }

    return idx < CMOS_RAM_SIZE ? cmos.reg[idx] : 0xFF;
}

void cmos_write_data(const uint8_t v) {
    const uint8_t idx = cmos.index;
    if (idx >= CMOS_RAM_SIZE) return;

    switch (idx) {
        case CMOS_STATUS_C:
        case CMOS_STATUS_D:
            return;                       // read-only status

        case CMOS_SECONDS: case CMOS_MINUTES: case CMOS_HOURS:
        case CMOS_DAY_OF_WEEK: case CMOS_DAY_OF_MONTH:
        case CMOS_MONTH: case CMOS_YEAR: case CMOS_CENTURY:
            cmos.reg[idx] = v;
            // Flagged, not written. This runs in the bus interrupt with
            // the 8086 in a wait state; the I2C transaction happens on
            // core 0 in cmos_tick(). Setting the date touches several
            // registers, so the flush deliberately lags by a tick and
            // sends one consistent value rather than six partial ones.
            cmos.dirty = true;
            return;

        default:
            cmos.reg[idx] = v;
            return;
    }
}
