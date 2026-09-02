/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The on-screen display: swap drives without rebooting.
 *
 * SETUP can only run before the 8086 does, because it draws into the same
 * text page the guest owns. This does not: it has its own 80x30 buffer,
 * and while it is up the VGA text renderer reads from that instead of
 * VIDEORAM. The guest carries on running underneath, writing a screen
 * nobody is looking at, and gets it back untouched when the menu closes.
 *
 * The 8086 is not paused, and cannot be. Its HOLD pin is strapped to
 * ground on this board and minimum mode gives no other way to stall it
 * for an unbounded time, so "pausing" would mean holding READY low across
 * a bus cycle and hoping nothing times out. Letting it run is both easier
 * and more honest: a guest that was mid-disk-read simply waits, exactly
 * as it would for a slow drive.
 *
 * Opened by Ctrl+Alt+F1, which the keyboard path swallows rather than
 * passing on -- see handleScancode() in app/main.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Is the menu currently on screen?
bool osd_active(void);

/*
 * Open the drive menu and run it to completion.
 *
 * Blocking: it does not return until the operator closes it. Core 0 is
 * therefore not running its main loop, so this pumps the two duties that
 * cannot wait — audio, which would underrun into a buzz, and the
 * keyboard, which is how the menu is driven at all.
 *
 * DMA deliberately is not pumped. A floppy transfer in flight stalls
 * until the menu closes, which the guest experiences as a slow disk.
 */
void osd_drive_menu(void);
