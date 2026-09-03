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
 * Opened by Win+F11, which the keyboard path swallows rather than passing
 * on -- see handleScancode() in app/main.c. Win+F12 opens the settings
 * menu the same way; see osd_settings_menu() below.
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

/*
 * Open SETUP live, over a running guest, and reboot if anything changed.
 *
 * SETUP itself does not know it is being called this way: it is the same
 * function the splash screen runs before the 8086 exists, just given the
 * OSD's private buffer to draw into instead of VIDEORAM and the guest
 * paused instead of not yet started. See setup_menu() in ui/setup_menu.h.
 *
 * Most of what SETUP edits -- the drive images, the clock, sound on or
 * off -- takes effect on its own the moment settings changes, the same
 * as it always has. A few fields do not: the RAM ceiling a VGA or
 * Hercules card takes, and the 8086's own clock speed, are read once at
 * boot, before this function or anything like it exists. DOS counts
 * memory once, at its own boot, and would not notice a ceiling moving
 * under it.
 *
 * So the choice here is not "reboot on any change" -- it is "reboot",
 * full stop, whenever anything was saved, because this function has no
 * way to tell a memory-affecting change from an harmless one and a
 * reboot is cheap. reset_cpu() gives the 8086 the fresh boot those fields
 * need; apply_boot_time_settings() in app/main.c is what recomputes them
 * first, so the boot that follows counts memory correctly rather than
 * against whatever ceiling the previous session left behind.
 */
void osd_settings_menu(void);
