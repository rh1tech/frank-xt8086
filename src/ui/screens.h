/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The three screens the firmware shows before the 8086 has said anything:
 * the splash, a fatal error, and a warning.
 *
 * All three exist because the prototype had nowhere to put a failure. It
 * mounted the SD card before it brought video up, so the single most
 * common bring-up fault — no card, or a card with no images — produced a
 * dark screen and a board that had dropped off the USB bus into BOOTSEL.
 * Video now comes up first and these are what it shows.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// What the splash reports about the machine it is running on. Filled in by
// main() as each subsystem comes up, so the splash can say what was found
// rather than what was configured.
typedef struct {
    uint32_t cpu_mhz;         // RP2350 system clock
    uint32_t psram_mhz;       // QSPI PSRAM clock ceiling
    uint32_t psram_bytes;     // 0 if the PSRAM did not answer
    bool     sd_ok;           // card mounted
    bool     cpu8086_present; // the 8086 fetched its reset vector
    bool     cpu8086_seen;    // ...or at least drove a memory cycle
    uint32_t cpu8086_addr;    // the address it drove, when that went wrong
    uint32_t cpu8086_khz;     // clock it is being given
} splash_info_t;

/*
 * The startup splash: a centred window over an animated plasma, held for
 * `hold_ms` or until a key is pressed, whichever comes first.
 *
 * Returns true if a key was pressed — the caller uses that to decide
 * whether the operator is present, and so whether SETUP should wait for
 * them or carry on and boot.
 */
bool screen_splash(const splash_info_t *info, uint32_t hold_ms);

/*
 * A fatal error: red box, message, detail, and no way out. Does not
 * return. Also written to the console, because a board with no display
 * attached still has J1.
 */
[[noreturn]] void screen_fatal(const char *title, const char *message,
                               const char *detail);

/*
 * A warning: the same shape in brown, shown for `hold_ms` and then
 * dismissed. Execution continues. Use for the faults that leave a usable
 * machine — no SD card is one, since the built-in bootOS still runs.
 */
void screen_warning(const char *title, const char *message,
                    const char *detail, uint32_t hold_ms);
