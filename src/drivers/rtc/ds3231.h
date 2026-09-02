/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The DS3231MZ (U15) on I2C1, GP38/GP39.
 *
 * This is the only real timekeeping on the board: it has its own crystal,
 * a CR1220 behind it, and it carries the date across a power cycle. What
 * the 8086 sees is a MC146818 at ports 70h/71h — see chipset/mc146818.h —
 * and this driver is what puts real time behind that emulation.
 *
 * Hardware I2C, not bit-banged. GP38/GP39 are I2C1's SDA/SCL in the
 * RP2350 pin map, so the peripheral can drive them directly; frank-test
 * bit-bangs the same chip only because it has to work across boards whose
 * pins land nowhere near an I2C block. Here they do.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Wall-clock time, in the units people write dates in — not BCD, and not
// an epoch. The BCD conversions belong at the two edges that need them
// (the DS3231's registers and the CMOS model's), not in the middle.
typedef struct {
    uint16_t year;   // full year, 2000..2099
    uint8_t  mon;    // 1..12
    uint8_t  day;    // 1..31
    uint8_t  dow;    // 1..7, 1 = Sunday, as the DS3231 and MC146818 agree
    uint8_t  hour;   // 0..23, always 24-hour here
    uint8_t  min;    // 0..59
    uint8_t  sec;    // 0..59
} rtc_time_t;

/*
 * Bring up I2C1 and look for the chip.
 *
 * Returns false if nothing acknowledges at 0x68, which is a board with no
 * RTC fitted or a dead one — the caller carries on either way, because a
 * machine with a wrong clock is still a usable machine.
 */
bool ds3231_init(void);

// Was a chip found by ds3231_init()?
bool ds3231_present(void);

/*
 * Has the oscillator stopped since the time was last set?
 *
 * The DS3231 latches this in its status register and keeps it set until
 * software clears it, which makes it the honest answer to "is this date
 * real or is it whatever the register powered up as". It is how the CMOS
 * model decides whether to advertise valid time to the guest.
 */
bool ds3231_lost_power(void);

// Read the current time. False on a bus error; *t is then untouched.
bool ds3231_read(rtc_time_t *t);

/*
 * Set the time, and clear the oscillator-stopped flag: the guest telling
 * us the date is exactly the event that makes it trustworthy again.
 *
 * The day-of-week field of *t is ignored and recomputed from the date.
 * DOS writes the date through INT 1Ah without touching the weekday, so
 * honouring a caller's dow means storing a date and a weekday that
 * disagree.
 */
bool ds3231_write(const rtc_time_t *t);

// ---------------------------------------------------------------------------
// Calendar helpers
// ---------------------------------------------------------------------------
//
// Taken from frank-test's core/rtc_ds3231.c, which drives the same part on
// the same board. Same behaviour, so the fleet agrees on what a valid date
// is rather than each firmware deciding separately.

uint8_t rtc_days_in_month(uint16_t year, uint8_t mon);

// Is this a date the DS3231 can hold and a calendar recognises?
bool rtc_time_valid(const rtc_time_t *t);

// Day of week for a date, 1..7 with Sunday = 1, as both the DS3231 and
// the MC146818 number it.
uint8_t rtc_day_of_week(uint16_t year, uint8_t mon, uint8_t day);

/*
 * The moment this firmware was compiled, as a last-resort clock.
 *
 * Wrong, but wrong in a bounded and forward-only way: it can never claim
 * a date before the software existed, which is a better starting point
 * for a filesystem than the 1980 an unset RTC would otherwise hand DOS.
 */
void rtc_build_time(rtc_time_t *out);

/*
 * Seconds since 2000-01-01 00:00:00, and back.
 *
 * Only used to add elapsed time to a date, which is fiddly to do in
 * place: month lengths vary and February needs the leap rule. Going
 * through a linear count keeps the rollover logic in one function
 * instead of scattered across the tick path.
 */
uint32_t rtc_to_epoch(const rtc_time_t *t);
void     rtc_from_epoch(uint32_t secs, rtc_time_t *out);
