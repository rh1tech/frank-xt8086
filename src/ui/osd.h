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
 * The 8086 is stopped for as long as the menu is up.
 *
 * This said for a while that it could not be, on the grounds that HOLD is
 * strapped to ground. That was the wrong pin: READY is ours, and holding
 * it low parks the CPU in wait states indefinitely -- which is already
 * what every IDE sector read does for milliseconds at a time. There is no
 * bus timeout on an 8086 to lose, and system memory is PSRAM rather than
 * DRAM waiting on refresh.
 *
 * It matters because this menu browses the card on core 0 while the hard
 * disk model browses it on core 1, and FatFs has a single window buffer
 * per volume. Interleaving them corrupted it: f_opendir() returned
 * FR_NO_PATH for directories that plainly existed, and picking a file
 * crashed the machine. Stopping the guest removes the concurrency rather
 * than trying to survive it. See bus_pause() in core/cpu_bus.c.
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
