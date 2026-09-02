/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ds3231.h"

#include <stddef.h>

#include <hardware/gpio.h>
#include <pico/time.h>
#include <hardware/i2c.h>

#include "xt8086.h"   // I2C_SDA_PIN, I2C_SCL_PIN

#define DS3231_ADDR      0x68u
#define DS3231_REG_SECS  0x00u
#define DS3231_REG_STAT  0x0Fu
#define DS3231_STAT_OSF  0x80u

// I2C1, because GP38/GP39 are its SDA/SCL. 100 kHz: the part does 400,
// but nothing here is in a hurry — the CMOS model reads this once a
// second — and 100 kHz is kinder to a bus with 4.7k pull-ups and a
// battery-backed part on the end of it.
#define RTC_I2C          i2c1
#define RTC_I2C_HZ       100000

// Every timekeeping register in a DS3231 is BCD.
static unsigned from_bcd(const uint8_t v) { return (v >> 4) * 10u + (v & 0x0Fu); }
static uint8_t  to_bcd(const unsigned v)  { return (uint8_t)(((v / 10u) << 4) | (v % 10u)); }

static bool present;
static bool lost_power;

// ---------------------------------------------------------------------------
// Bus helpers
// ---------------------------------------------------------------------------
//
// Every transfer is bounded by a timeout. A bus wedged by a half-clocked
// device would otherwise block core 0 forever, and core 0 is what feeds
// the 8086's DMA and video — a stuck RTC must not take the machine down
// with it.
#define RTC_TIMEOUT_US 5000

static bool reg_read(const uint8_t reg, uint8_t *buf, const size_t len) {
    if (i2c_write_timeout_us(RTC_I2C, DS3231_ADDR, &reg, 1, true, RTC_TIMEOUT_US) != 1)
        return false;
    return i2c_read_timeout_us(RTC_I2C, DS3231_ADDR, buf, len, false, RTC_TIMEOUT_US)
           == (int)len;
}

static bool reg_write(const uint8_t reg, const uint8_t *buf, const size_t len) {
    uint8_t tmp[8];
    if (len + 1 > sizeof tmp) return false;
    tmp[0] = reg;
    for (size_t i = 0; i < len; i++) tmp[i + 1] = buf[i];
    return i2c_write_timeout_us(RTC_I2C, DS3231_ADDR, tmp, len + 1, false, RTC_TIMEOUT_US)
           == (int)(len + 1);
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

/*
 * Unstick the bus before trusting it.
 *
 * I2C has no reset line. If the RP2350 is reset part-way through a
 * transfer -- and cmos_tick() starts one every second, so the odds over a
 * few reflashes are good -- the DS3231 is left in the middle of a byte
 * still holding SDA low, waiting for clocks that never come. Nothing on
 * the master side clears that: i2c_init() only configures our end, so
 * every read afterwards fails and the part reads as absent. It survives
 * reset after reset, because only the slave can let go, and it will not
 * until it is clocked.
 *
 * That is exactly what happened here: a chip that had been reporting its
 * time perfectly well became "DS3231 absent" and stayed that way.
 *
 * The remedy is the standard one. Take the pins back as GPIO and, while
 * SDA is held low, clock SCL -- at most nine times, which is one byte
 * plus its acknowledge, the longest a slave can still be waiting. Then
 * frame a STOP by hand so it starts from a known state.
 */
static void i2c_bus_recover(void) {
    gpio_init(I2C_SCL_PIN);
    gpio_init(I2C_SDA_PIN);

    // SDA as an input: whether the slave is holding it is the question.
    gpio_set_dir(I2C_SDA_PIN, GPIO_IN);
    gpio_set_dir(I2C_SCL_PIN, GPIO_OUT);
    gpio_put(I2C_SCL_PIN, 1);
    sleep_us(5);

    for (int i = 0; i < 9 && !gpio_get(I2C_SDA_PIN); i++) {
        gpio_put(I2C_SCL_PIN, 0);
        sleep_us(5);
        gpio_put(I2C_SCL_PIN, 1);
        sleep_us(5);
    }

    // STOP is SDA rising while SCL is high.
    gpio_set_dir(I2C_SDA_PIN, GPIO_OUT);
    gpio_put(I2C_SDA_PIN, 0);
    sleep_us(5);
    gpio_put(I2C_SCL_PIN, 1);
    sleep_us(5);
    gpio_put(I2C_SDA_PIN, 1);
    sleep_us(5);
    gpio_set_dir(I2C_SDA_PIN, GPIO_IN);
}

bool ds3231_init(void) {
    i2c_bus_recover();

    i2c_init(RTC_I2C, RTC_I2C_HZ);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    // The board has 4.7k pull-ups on both lines; the internal ones are
    // ~50k and would only weaken the edges if they fought them.

    uint8_t status;
    present = reg_read(DS3231_REG_STAT, &status, 1);
    lost_power = present && (status & DS3231_STAT_OSF);
    return present;
}

bool ds3231_present(void)    { return present; }
bool ds3231_lost_power(void) { return lost_power; }

bool ds3231_read(rtc_time_t *t) {
    if (!present || !t) return false;

    uint8_t r[7];
    if (!reg_read(DS3231_REG_SECS, r, sizeof r)) return false;

    t->sec  = (uint8_t)from_bcd(r[0] & 0x7Fu);
    t->min  = (uint8_t)from_bcd(r[1] & 0x7Fu);

    // Bit 6 of the hours register selects 12-hour mode, and then bit 5 is
    // PM rather than the twenties digit. We always write 24-hour, but a
    // chip programmed by something else can still be in 12-hour mode and
    // reading it as 24 would silently mangle the afternoon.
    if (r[2] & 0x40u) {
        unsigned h = from_bcd(r[2] & 0x1Fu) % 12u;
        if (r[2] & 0x20u) h += 12u;
        t->hour = (uint8_t)h;
    } else {
        t->hour = (uint8_t)from_bcd(r[2] & 0x3Fu);
    }

    t->dow  = (uint8_t)(r[3] & 0x07u);
    t->day  = (uint8_t)from_bcd(r[4] & 0x3Fu);
    t->mon  = (uint8_t)from_bcd(r[5] & 0x1Fu);

    // Bit 7 of the month register is the century flag. The DS3231 toggles
    // it on a 99->00 rollover, so it is a single bit of century, not a
    // number: 2000..2099 unless it has rolled once.
    t->year = (uint16_t)(from_bcd(r[6]) + ((r[5] & 0x80u) ? 2100u : 2000u));
    return true;
}

bool ds3231_write(const rtc_time_t *t) {
    if (!present || !t) return false;

    if (!rtc_time_valid(t)) return false;

    const unsigned yr = t->year >= 2100u ? t->year - 2100u : t->year - 2000u;
    // Seconds first and all seven in one burst: the part auto-increments,
    // so the date lands atomically and the clock is never seen half-set.
    const uint8_t r[7] = {
        to_bcd(t->sec),
        to_bcd(t->min),
        to_bcd(t->hour),                                  // bit 6 clear = 24-hour
        rtc_day_of_week(t->year, t->mon, t->day),
        to_bcd(t->day),
        (uint8_t)(to_bcd(t->mon) | (t->year >= 2100u ? 0x80u : 0x00u)),
        to_bcd(yr),
    };
    if (!reg_write(DS3231_REG_SECS, r, sizeof r)) return false;

    // Someone has just told us what time it is, so the oscillator-stopped
    // flag has served its purpose. Clearing it is what makes the next boot
    // trust the date instead of reporting a dead battery.
    uint8_t status;
    if (reg_read(DS3231_REG_STAT, &status, 1)) {
        status &= (uint8_t)~DS3231_STAT_OSF;
        reg_write(DS3231_REG_STAT, &status, 1);
        lost_power = false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Calendar helpers
// ---------------------------------------------------------------------------

uint8_t rtc_days_in_month(const uint16_t year, const uint8_t mon) {
    static const uint8_t len[12] = { 31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31 };
    if (mon < 1u || mon > 12u) return 31u;
    if (mon == 2u) {
        const bool leap = (year % 4u == 0u) &&
                          (year % 100u != 0u || year % 400u == 0u);
        return leap ? 29u : 28u;
    }
    return len[mon - 1u];
}

bool rtc_time_valid(const rtc_time_t *t) {
    if (!t) return false;
    if (t->year < 2000u || t->year > 2099u) return false;
    if (t->mon < 1u || t->mon > 12u) return false;
    if (t->day < 1u || t->day > rtc_days_in_month(t->year, t->mon)) return false;
    if (t->hour > 23u || t->min > 59u || t->sec > 59u) return false;
    return true;
}

uint8_t rtc_day_of_week(const uint16_t year, const uint8_t mon, const uint8_t day) {
    // Counted forward from a known anchor rather than by Zeller, because
    // the leap rule is already written down in rtc_days_in_month() and one
    // definition of the calendar is enough.
    // 2000-01-01 was a Saturday, which in this numbering (1 = Sunday, as
    // both the DS3231 and the MC146818 count) is 7, not 6.
    //
    // frank-test's core/rtc_ds3231.c has 6 here with the same comment, so
    // every weekday it derives is one day early. Nothing there reads the
    // field back, which is why it has gone unnoticed; here DOS can read it
    // through CMOS register 6, so it has to be right.
    static const uint8_t dow_ref = 7u;
    unsigned days = 0;
    for (uint16_t y = 2000u; y < year; y++)
        days += ((y % 4u == 0u) && (y % 100u != 0u || y % 400u == 0u)) ? 366u : 365u;
    for (uint8_t m = 1u; m < mon; m++)
        days += rtc_days_in_month(year, m);
    days += (unsigned)(day - 1u);
    return (uint8_t)(((days + dow_ref - 1u) % 7u) + 1u);
}

void rtc_build_time(rtc_time_t *out) {
    if (!out) return;

    // __DATE__ is "Mmm dd yyyy" and __TIME__ is "hh:mm:ss", both fixed
    // width and both guaranteed by the standard. Parsed rather than
    // preprocessed into a number because there is no way to do the latter
    // in C without a build step, and this costs nothing.
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    const char *t = __TIME__;

    uint8_t mon = 1;
    for (uint8_t i = 0; i < 12u; i++) {
        if (d[0] == months[i * 3] && d[1] == months[i * 3 + 1] &&
            d[2] == months[i * 3 + 2]) { mon = (uint8_t)(i + 1u); break; }
    }

    out->mon  = mon;
    // The day is space-padded below ten, so the tens digit may not be one.
    out->day  = (uint8_t)((d[4] == ' ' ? 0u : (unsigned)(d[4] - '0') * 10u)
                          + (unsigned)(d[5] - '0'));
    out->year = (uint16_t)((d[7] - '0') * 1000 + (d[8] - '0') * 100 +
                           (d[9] - '0') * 10 + (d[10] - '0'));
    out->hour = (uint8_t)((t[0] - '0') * 10 + (t[1] - '0'));
    out->min  = (uint8_t)((t[3] - '0') * 10 + (t[4] - '0'));
    out->sec  = 0;

    if (!rtc_time_valid(out)) {
        const rtc_time_t fallback = { 2026u, 1u, 1u, 1u, 0u, 0u, 0u };
        *out = fallback;
    }
    out->dow = rtc_day_of_week(out->year, out->mon, out->day);
}

uint32_t rtc_to_epoch(const rtc_time_t *t) {
    if (!t) return 0;
    unsigned days = 0;
    for (uint16_t y = 2000u; y < t->year; y++)
        days += ((y % 4u == 0u) && (y % 100u != 0u || y % 400u == 0u)) ? 366u : 365u;
    for (uint8_t m = 1u; m < t->mon; m++)
        days += rtc_days_in_month(t->year, m);
    days += (unsigned)(t->day - 1u);
    return (uint32_t)days * 86400u + t->hour * 3600u + t->min * 60u + t->sec;
}

void rtc_from_epoch(uint32_t secs, rtc_time_t *out) {
    if (!out) return;
    unsigned days = secs / 86400u;
    const unsigned rem = secs % 86400u;

    out->hour = (uint8_t)(rem / 3600u);
    out->min  = (uint8_t)((rem % 3600u) / 60u);
    out->sec  = (uint8_t)(rem % 60u);

    uint16_t y = 2000u;
    for (;;) {
        const unsigned len = ((y % 4u == 0u) && (y % 100u != 0u || y % 400u == 0u)) ? 366u : 365u;
        if (days < len) break;
        days -= len;
        y++;
    }
    out->year = y;

    uint8_t m = 1u;
    for (;;) {
        const unsigned len = rtc_days_in_month(y, m);
        if (days < len) break;
        days -= len;
        m++;
    }
    out->mon = m;
    out->day = (uint8_t)(days + 1u);
    out->dow = rtc_day_of_week(out->year, out->mon, out->day);
}
