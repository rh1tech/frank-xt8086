/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Is there an 8086 in the socket, and how fast will this one go?
 *
 * Two questions with very different costs, so two functions.
 *
 * Presence is nearly free: release the CPU from reset and watch for its
 * first fetch. An 8086 comes out of reset with CS:IP = FFFF:0000 and
 * fetches physical 0xFFFF0 within a handful of clocks, so seeing that
 * address is proof of a live part — it can only be produced by something
 * that decoded a reset and drove a bus cycle.
 *
 * Speed is not free. 8086s were sold in grades — 5 MHz for a plain 8086,
 * 8 for an 8086-2, 10 for an 8086-1 — and parts vary within a grade, so
 * the only way to know what the one in this socket does is to run it and
 * see where it stops. That means resetting the machine repeatedly, which
 * is a SETUP action, not a boot step.
 *
 * What the sweep measures is genuinely the CPU, not this firmware. The
 * 8086's clock is its own PWM on GP29, independent of the RP2350's system
 * clock, and READY stretches every bus cycle until we answer — so our
 * service latency costs the guest throughput, never correctness. Raise
 * the clock far enough and what fails is the part's own internal timing.
 *
 * The honest caveat is that the failure could be the board rather than the
 * die: the AD bus is a long unbuffered run and it gives out somewhere too.
 * The number therefore means "this CPU on this board", which is the number
 * that matters when choosing what to run it at anyway.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// The address an 8086 fetches first, from the reset vector at FFFF:0000.
#define I8086_RESET_VECTOR 0xFFFF0u

typedef struct {
    bool     present;      // a bus cycle was seen
    bool     vector_ok;    // ...and it was for I8086_RESET_VECTOR
    uint32_t first_addr;   // whatever it did fetch, for a failure report
    uint32_t cycles;       // bus cycles counted during the probe window
} cpu_probe_result_t;

/*
 * Look for a CPU, at `khz`, within `timeout_ms`.
 *
 * Self-contained: brings up the bus state machine, drives the clock and
 * reset, watches the PIO's receive FIFO directly rather than through the
 * interrupt handlers, and leaves the CPU held in reset with the state
 * machine stopped. Safe to call before cpu_bus_init(), and it must be —
 * the real handlers live on core 1 and this runs on core 0 during boot.
 */
cpu_probe_result_t cpu_probe(uint32_t khz, uint32_t timeout_ms);

// The speed grades an 8086 was sold in, for reporting a sweep result.
const char *cpu_probe_grade_name(uint32_t khz);
