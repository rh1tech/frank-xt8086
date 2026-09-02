/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fslock.h"

// The one instance. See fslock.h for why this cannot live in the header.
spin_lock_t *fs_lock_ptr;

void fs_lock_init(void) {
    if (!fs_lock_ptr) fs_lock_ptr = spin_lock_instance(spin_lock_claim_unused(true));
}
