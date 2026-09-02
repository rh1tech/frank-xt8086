/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The APS6404-class PSRAM (U8, ESP-PSRAM64H, 8 MB) hangs off the same QSPI
 * bus as the boot flash, selected by GP47 as XIP chip select 1. Bringing it
 * up is a QMI exercise, not a bus-driver one: put the part into QPI mode
 * through direct mode, program the M1 timing and read/write formats, then
 * hand the window back to XIP so RAM[] can simply be a linker section.
 *
 * This lived as a 75-line static inline in the old common.h, which meant
 * every translation unit that wanted a pin number also parsed a QMI setup
 * routine and got its own dead copy. It is a driver; it lives with drivers.
 */
#pragma once

/*
 * Bring up the PSRAM on XIP CS1 and make the window writable.
 *
 * cs_pin        the GPIO wired to the PSRAM's chip select (GP47 here)
 * max_freq_hz   ceiling for the QMI clock; the divisor is derived from the
 *               live clk_sys, so this stays correct across CPU_SPEED changes
 */
void psram_init(int cs_pin, unsigned max_freq_hz);
