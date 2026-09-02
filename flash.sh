#!/usr/bin/env bash
#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# flash.sh — flash over USB with picotool.
#
# The fallback for when no probe is attached. Prefer swd_flash.sh: this
# path needs the board on the USB bus, and the moment the firmware wedges
# hard enough to stop enumerating, it needs S3/S4 pressed by hand — which
# is exactly when you are iterating fastest.
#
set -euo pipefail
cd "$(dirname "$0")"

UF2="${1:-app/build/frank-xt8086.uf2}"

if [[ ! -f "${UF2}" ]]; then
    echo "ERROR: ${UF2} not found. Run ./build.sh first." >&2
    exit 1
fi

echo "Flashing ${UF2}"
# -f forces a running target into BOOTSEL; -x runs the new image. No
# separate `picotool reboot` afterwards — that resets the board mid
# enumeration and the host never sees it come back.
picotool load -fx "${UF2}"
