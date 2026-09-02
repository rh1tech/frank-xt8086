/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio.h"

#include <stdio.h>
#include <string.h>

#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pwm.h>
#include <pico/time.h>

#include "adlib.h"
#include "xt8086.h"   // BEEPER_PIN

// 8-bit duty resolution: wrap 255 at clkdiv 1 puts the carrier at
// sys_clk/256, about 1.97 MHz at 504 MHz. Far enough above the audio band
// that the transducer and the HDMI encoder both average it away, and
// still eight full bits of level.
#define PWM_WRAP         255u
#define PWM_MID          128u

// The PIT's input clock, as the 8253 model uses it.
#define PIT_HZ           1193182u

static uint     slice, chan;
static int      dma_a = -1, dma_b = -1;
static uint     pacer;

// Two buffers of PWM levels, ping-ponged by the two DMA channels.
static uint16_t buf[2][AUDIO_BLOCK];

// Which buffer the DMA is *not* playing, and so is ours to refill.
static volatile int  refill = -1;
static volatile bool running;

// Scratch for the OPL. 32-bit: EMU8950_LINEAR renders at that width.
static int32_t opl_samples[AUDIO_BLOCK];

// --- speaker state, written from the bus interrupt -------------------------
static volatile uint16_t spk_reload = 0;
static volatile bool     spk_on     = false;

// Square-wave phase, advanced per sample on core 0.
static uint32_t spk_phase;

void audio_speaker_divisor(const uint16_t reload) { spk_reload = reload; }
void audio_speaker_gate(const bool on)            { spk_on = on; }

// ---------------------------------------------------------------------------
// DMA
// ---------------------------------------------------------------------------

/*
 * DMA_IRQ_1, not _0.
 *
 * The VGA driver installs an *exclusive* handler on DMA_IRQ_0 for its
 * scanline chain. Adding a shared handler there first makes its
 * irq_set_exclusive_handler() panic -- and with PICO_PANIC_FUNCTION=0 a
 * panic is a silent hang, so the symptom is simply that the boot stops
 * after the audio line with nothing to say why.
 *
 * The two IRQs are equivalent for our purposes, so audio takes the one
 * video does not.
 */
static void dma_isr(void) {
    // Whichever channel just finished, the buffer it drained is now ours.
    if (dma_hw->ints1 & (1u << dma_a)) {
        dma_hw->ints1 = 1u << dma_a;
        dma_channel_set_read_addr(dma_a, buf[0], false);  // rearm for next time
        refill = 0;
    }
    if (dma_hw->ints1 & (1u << dma_b)) {
        dma_hw->ints1 = 1u << dma_b;
        dma_channel_set_read_addr(dma_b, buf[1], false);
        refill = 1;
    }
}

void audio_init(void) {
    slice = pwm_gpio_to_slice_num(BEEPER_PIN);
    chan  = pwm_gpio_to_channel(BEEPER_PIN);

    gpio_set_function(BEEPER_PIN, GPIO_FUNC_PWM);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&c, 1);
    pwm_config_set_wrap(&c, PWM_WRAP);
    pwm_init(slice, &c, true);
    pwm_set_chan_level(slice, chan, PWM_MID);

    for (int i = 0; i < 2; i++)
        for (uint32_t s = 0; s < AUDIO_BLOCK; s++) buf[i][s] = PWM_MID;

    /*
     * Pacing.
     *
     * The DMA's fractional timer produces DREQs at sys_clk * num/den, and
     * both fields are 16 bits — so 44100/504000000 cannot be expressed
     * directly: reduced it is 7/80000, and 80000 does not fit. 1/11429
     * gives 44098.3 Hz on this clock, which is 0.004 % low and inaudible.
     *
     * Derived from the live clock rather than the compile-time constant so
     * a CPU_SPEED change does not silently retune every sound the machine
     * makes.
     */
    pacer = dma_claim_unused_timer(true);
    const uint32_t den = (clock_get_hz(clk_sys) + AUDIO_RATE_HZ / 2) / AUDIO_RATE_HZ;
    dma_timer_set_fraction(pacer, 1, (uint16_t)(den > 0xFFFFu ? 0xFFFFu : den));

    dma_a = dma_claim_unused_channel(true);
    dma_b = dma_claim_unused_channel(true);

    // Each channel plays one buffer into the PWM compare register, then
    // chains to the other. The pair free-runs; the interrupt only says
    // which buffer became ours, it does not restart anything.
    volatile void *cc = &((uint16_t *)&pwm_hw->slice[slice].cc)[chan];

    for (int i = 0; i < 2; i++) {
        const int ch    = i ? dma_b : dma_a;
        const int other = i ? dma_a : dma_b;
        dma_channel_config cfg = dma_channel_get_default_config(ch);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
        channel_config_set_read_increment(&cfg, true);
        channel_config_set_write_increment(&cfg, false);
        channel_config_set_dreq(&cfg, dma_get_timer_dreq(pacer));
        channel_config_set_chain_to(&cfg, other);
        dma_channel_configure(ch, &cfg, cc, buf[i], AUDIO_BLOCK, false);
        dma_channel_set_irq1_enabled(ch, true);
    }

    irq_set_exclusive_handler(DMA_IRQ_1, dma_isr);
    irq_set_enabled(DMA_IRQ_1, true);

    running = true;
    dma_channel_start(dma_a);

    printf("[audio] PWM DAC on GP%d, %lu Hz, %u-sample blocks\n",
           BEEPER_PIN, (unsigned long)(clock_get_hz(clk_sys) / den), (unsigned)AUDIO_BLOCK);
}

// ---------------------------------------------------------------------------
// Mixing
// ---------------------------------------------------------------------------

// Render one block into buf[which].
static void fill(const int which) {
    const bool have_opl = adlib_render(opl_samples, AUDIO_BLOCK);

    // The speaker's half-period in samples. A reload of 0 means 65536 on a
    // real 8253, and anything below a handful is ultrasonic anyway; both
    // end up silent rather than as a divide-by-zero.
    const uint32_t reload = spk_reload ? spk_reload : 65536u;
    const uint32_t rate   = clock_get_hz(clk_sys) / ((clock_get_hz(clk_sys) + AUDIO_RATE_HZ / 2) / AUDIO_RATE_HZ);
    const uint32_t half   = (uint32_t)(((uint64_t)reload * rate) / (PIT_HZ * 2u));
    const bool     beep   = spk_on && half > 0;

    for (uint32_t i = 0; i < AUDIO_BLOCK; i++) {
        int32_t s = 0;

        if (have_opl) s += opl_samples[i];

        if (beep) {
            // A quarter of full scale. The speaker was always the loudest
            // thing on a PC and drowning the OPL under it would be
            // faithful but unpleasant.
            s += (spk_phase < half) ? 8192 : -8192;
            if (++spk_phase >= half * 2u) spk_phase = 0;
        }

        // Signed 16-bit to an 8-bit unsigned duty, clipped rather than
        // wrapped: a wrap turns a loud passage into a burst of noise, and
        // clipping at least sounds like the thing that was too loud.
        s >>= 8;
        if (s >  127) s =  127;
        if (s < -128) s = -128;
        buf[which][i] = (uint16_t)(s + PWM_MID);
    }
}

void audio_task(void) {
    const int which = refill;
    if (which < 0) return;
    refill = -1;
    fill(which);
}

void audio_beep(const uint32_t freq_hz, const uint32_t ms) {
    if (!running || !freq_hz) return;

    // Drive the speaker model directly, then hand it back. This runs
    // before the 8086 exists, so there is nothing to conflict with.
    const uint16_t saved_reload = spk_reload;
    const bool     saved_gate   = spk_on;

    spk_reload = (uint16_t)(PIT_HZ / freq_hz);
    spk_on     = true;
    spk_phase  = 0;

    const absolute_time_t until = make_timeout_time_ms(ms);
    while (absolute_time_diff_us(get_absolute_time(), until) > 0) audio_task();

    spk_reload = saved_reload;
    spk_on     = saved_gate;
    // One more block so the buffer that is about to play is silent rather
    // than the tail of the tone repeating until something else fills it.
    audio_task();
}
