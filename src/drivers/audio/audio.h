/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * One speaker pin, two sound sources.
 *
 * GP46 is all the audio this board has — JP1/JP2/JP3 route it to the HDMI
 * encoder, the 3.5 mm jack or the buzzer, one at a time. The PC speaker
 * and the OPL2 both want it, so it stops being a square-wave output and
 * becomes a PWM DAC: a ~1.97 MHz carrier whose duty cycle is rewritten at
 * the sample rate by DMA, with both sources mixed in software.
 *
 * That is a strict improvement even ignoring the OPL. The speaker was a
 * hardware square wave whose frequency came from reprogramming the PWM
 * wrap register, which meant the PIT's channel-2 reload value and the
 * PWM's resolution were the same number — at a typical DOS reload of 1193
 * the duty resolution was eleven bits, and at a high-pitched 100 it was
 * seven. Synthesising the square wave instead decouples the two.
 *
 * Timing comes from a DMA pacing timer rather than a PWM wrap interrupt,
 * so nothing here needs servicing per sample: two DMA channels ping-pong
 * between two buffers, chained to each other, and core 0 refills whichever
 * one just drained.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 44.1 kHz. The OPL2's own output is band-limited well below this and
// core 0 has the headroom at 504 MHz; halve it first if anything starts
// missing frames.
#define AUDIO_RATE_HZ    44100u

// Samples per buffer. 256 is ~5.8 ms of latency and ~170 refills a
// second, which is comfortably slower than the 60 Hz frame tick that
// drives everything else on core 0.
#define AUDIO_BLOCK      256u

/*
 * Take over GP46 and start the DAC running.
 *
 * Claims two DMA channels and one DMA pacing timer. Must run before core
 * 1 releases the CPU: it allocates, and it reprograms the pin the guest
 * can start writing to the moment the 8086 is out of reset.
 */
void audio_init(void);

/*
 * Refill whichever buffer has drained. Core 0 only.
 *
 * Returns immediately when neither buffer is due, so it is cheap to call
 * from the main loop. Everything expensive — OPL synthesis, mixing —
 * happens here rather than in an interrupt.
 */
void audio_task(void);

// ---------------------------------------------------------------------------
// What the emulated chips tell us
// ---------------------------------------------------------------------------
//
// Both of these are called from the bus interrupt, so both do nothing but
// store a number. The waveform is generated later, on core 0.

// PIT channel 2's reload value: the speaker's frequency divisor.
void audio_speaker_divisor(uint16_t reload);

// Port 61h bits 0 and 1: the speaker gate.
void audio_speaker_gate(bool on);

/*
 * A tone straight out of the mixer, for the boot chime.
 *
 * Bypasses the guest entirely — the speaker model belongs to the 8086 and
 * this runs before the 8086 exists. Blocking, and only ever called during
 * bring-up.
 */
void audio_beep(uint32_t freq_hz, uint32_t ms);

/*
 * Silence, or not.
 *
 * Muting fills the blocks with the idle level rather than stopping the
 * DMA. The carrier keeps running and the pin stays at mid-scale, so
 * switching back on makes no click and there is no half-configured state
 * to get wrong -- the speaker and the OPL go on being modelled, they are
 * simply not listened to.
 */
void audio_set_enabled(bool on);
