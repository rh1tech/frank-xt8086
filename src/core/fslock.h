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

/*
 * One lock, shared by every translation unit.
 *
 * This was a `static inline` function in this header wrapping a
 * function-local `static spin_lock_t *lock`. That gives each translation
 * unit its own copy of the variable, so main.c, setup_menu.c and the unit
 * holding ide.h each claimed a *different* spinlock and none of them ever
 * excluded any other. The lock read correctly and did nothing.
 *
 * It looked like it worked because the only path that had been exercised
 * was SETUP, which runs before core 1 is launched and therefore has no
 * contention to lose. The drive menu is the same browser with the guest
 * running underneath, and there f_opendir() started returning FR_NO_PATH:
 * core 1's hard disk reads were moving the shared FatFs window out from
 * under core 0's directory walk.
 *
 * fs_lock_init() must run before anything touches the filesystem and
 * before core 1 starts. main() calls it first thing.
 */
extern spin_lock_t *fs_lock_ptr;

void fs_lock_init(void);

/*
 * The *unsafe* variants, which is to say the ones that do not touch the
 * interrupt enable. That is deliberate and it matters.
 *
 * spin_lock_blocking() disables interrupts for as long as the lock is
 * held. The VGA scanline handler is a DMA interrupt on core 0 -- graphics
 * init runs there -- and holding this lock across an SD card read is
 * milliseconds. Blocking that interrupt for milliseconds stops the
 * scanline chain, the display loses sync, and the monitor drops the
 * signal entirely.
 *
 * Dropping the interrupt masking is safe here because nothing that runs
 * in an interrupt handler on the *same* core also takes this lock. On
 * core 0 it is taken only by the main loop -- floppy DMA, the browser,
 * media_reload, settings -- and none of core 0's handlers (VGA, audio,
 * USB) touch the filesystem. On core 1 it is taken only inside the bus
 * interrupt, and core 1's own loop never touches the filesystem. So there
 * is no same-core reentrancy for interrupt masking to protect against;
 * the only contention is between the two cores, which is what the
 * spinlock itself is for.
 */
#define FS_LOCK()    spin_lock_unsafe_blocking(fs_lock_ptr)
#define FS_UNLOCK()  spin_unlock_unsafe(fs_lock_ptr)
