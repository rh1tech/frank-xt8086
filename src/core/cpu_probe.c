/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cpu_probe.h"

#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <pico/time.h>

#include "i8086_bus.pio.h"
#include "state.h"

/*
 * Polling, not interrupts.
 *
 * cpu_bus_init() installs exclusive handlers for PIO0_IRQ_0 and _1 on
 * whichever core calls it, and that is core 1. This runs on core 0 during
 * boot, before core 1 exists, and claiming those vectors here would mean
 * releasing them again before core 1 could claim them — for a probe that
 * only ever needs to read one word out of the receive FIFO.
 *
 * The PIO program pushes the captured address as soon as ALE falls,
 * before it waits for a read or write strobe, so the first fetch is
 * visible in the FIFO without answering it. That is the whole probe.
 */
cpu_probe_result_t cpu_probe(const uint32_t khz, const uint32_t timeout_ms) {
    cpu_probe_result_t r = { 0 };

    /*
     * Order matters here, and getting it wrong produces a false negative
     * that looks exactly like a dead CPU.
     *
     * On a warm reset — the debugger restarting the RP2350, rather than
     * power being applied — the 8086 is not in a clean state. It was
     * mid-instruction when its clock stopped, and it sits there with
     * whatever it last drove still on the bus. Enabling the state machine
     * before that is cleared captures the stale cycle as if it were the
     * first fetch, and the probe reports an 8086 fetching 0x00000.
     *
     * So: assert reset and restore the clock first, let the part actually
     * see some clocks with reset held, and only then start listening.
     */
    gpio_init(RESET_PIN);
    gpio_set_dir(RESET_PIN, GPIO_OUT);
    gpio_put(RESET_PIN, 1);

    start_cpu_clock(khz);

    // The 8086 wants RESET held for at least four clocks. At the slowest
    // speed this firmware offers, 1 MHz, ten milliseconds is ten thousand.
    busy_wait_ms(10);

    const uint offset = pio_add_program(BUS_CTRL_PIO, &i8086_bus_program);
    i8086_bus_program_init(BUS_CTRL_PIO, BUS_CTRL_SM, offset);
    pio_sm_clear_fifos(BUS_CTRL_PIO, BUS_CTRL_SM);
    for (uint i = 0; i < 4; i++) pio_interrupt_clear(BUS_CTRL_PIO, i);
    pio_sm_set_enabled(BUS_CTRL_PIO, BUS_CTRL_SM, true);

    gpio_put(RESET_PIN, 0);

    const absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        if (!pio_sm_is_rx_fifo_empty(BUS_CTRL_PIO, BUS_CTRL_SM)) {
            const uint32_t bus_state = BUS_CTRL_PIO->rxf[BUS_CTRL_SM];
            const uint32_t addr = bus_state & 0xFFFFF;

            /*
             * Scan the window rather than trusting cycle zero.
             *
             * A capture is only interesting if M/IO says memory: the first
             * thing an 8086 does out of reset is a memory read, and it has
             * no reason to touch I/O space until a BIOS tells it to. That
             * one test discards the electrical noise that releasing RESET
             * puts on ALE, which would otherwise latch an all-zero address
             * off a bus nothing is driving yet.
             */
            if (!(bus_state & MIO)) continue;

            if (r.cycles++ == 0) {
                r.present    = true;
                r.first_addr = addr;
            }
            if (addr == I8086_RESET_VECTOR) r.vector_ok = true;
            // Keep draining so a CPU that is running does not wedge the
            // state machine on a full FIFO; the count is a liveness signal
            // in its own right. The CPU stays stalled in a wait state
            // because nothing answers its read, which is fine — we are
            // about to put it back in reset.
        }
    }

    // Leave the machine exactly as it was found: CPU in reset, state
    // machine stopped, program removed so cpu_bus_init() can load it at
    // whatever offset it likes.
    gpio_put(RESET_PIN, 1);
    pio_sm_set_enabled(BUS_CTRL_PIO, BUS_CTRL_SM, false);
    pio_sm_clear_fifos(BUS_CTRL_PIO, BUS_CTRL_SM);

    /*
     * Clear the program's interrupt flags, which is the part that is easy
     * to miss and fatal to skip.
     *
     * The probe answers nothing, so the state machine ends its one bus
     * cycle stalled in `pull block` inside RD_cycle — having already
     * raised IRQ 1 to ask for data. Disabling the state machine does not
     * clear that flag. It survives into cpu_bus_init(), which enables the
     * IRQ1 source and installs bus_read_handler(); the handler then fires
     * immediately, reads a word out of an empty receive FIFO, and pushes a
     * garbage response into the transmit FIFO. The first real fetch the
     * 8086 makes collects that stale word instead of its reset vector and
     * the CPU wanders off into whatever it decodes.
     *
     * Symptom, for the next person: the probe reports the CPU present and
     * the machine then never programs the CRTC.
     */
    for (uint i = 0; i < 4; i++) pio_interrupt_clear(BUS_CTRL_PIO, i);

    pio_remove_program(BUS_CTRL_PIO, &i8086_bus_program, offset);

    return r;
}

const char *cpu_probe_grade_name(const uint32_t khz) {
    // The part numbers Intel and its second sources shipped. A die that
    // passes above its marking is common and interesting; one that fails
    // below it is the reason this test exists.
    if (khz >= 10000) return "8086-1 grade (10 MHz)";
    if (khz >=  8000) return "8086-2 grade (8 MHz)";
    if (khz >=  5000) return "8086 grade (5 MHz)";
    return "below the 5 MHz grade";
}
