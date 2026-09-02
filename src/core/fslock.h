/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * One lock around FatFs, because two cores reach it.
 *
 * The hard disk model in chipset/ide.h calls f_lseek() and f_read() from
 * inside port_read() — which runs in the PIO bus interrupt, on core 1.
 * Everything else that touches the filesystem runs on core 0: the floppy
 * DMA in main(), the file browser, and media_reload(). FatFs has one
 * window buffer per volume and is not reentrant, so those two paths
 * overlapping corrupts its state.
 *
 * It went unnoticed for as long as core 0 only touched the card before
 * core 1 existed. The drive menu broke that: it browses the card while
 * the guest is running, and picking a file crashed the machine.
 *
 * A hardware spinlock rather than a mutex, because one side is an
 * interrupt handler that cannot sleep. Held per FatFs call, not across a
 * whole operation: each call leaves the filesystem consistent, so
 * interleaving between calls is safe, and core 1 never waits longer than
 * one call. The exception is media_reload(), which closes and reopens the
 * very files core 1 reads through and must therefore hold it throughout.
 *
 * Waiting in the bus interrupt is not the problem it looks like. The 8086
 * is held in a wait state for the duration, which is precisely what a
 * slow disk does to it, and an 8086 has no bus timeout to violate.
 */
#pragma once

#include <hardware/sync.h>

// PICO_SPINLOCK_ID_STRIPED_FIRST..LAST are handed out by the SDK; taking
// a striped one avoids colliding with anything the SDK claims itself.
static inline spin_lock_t *fs_spin(void) {
    static spin_lock_t *lock;
    if (!lock) lock = spin_lock_instance(spin_lock_claim_unused(true));
    return lock;
}

#define FS_LOCK()    const uint32_t _fs_irq = spin_lock_blocking(fs_spin())
#define FS_UNLOCK()  spin_unlock(fs_spin(), _fs_irq)
