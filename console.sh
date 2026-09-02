#!/usr/bin/env bash
#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# console.sh — open the firmware's UART console.
#
# stdio is UART0 on J1 (pin 1 = TX, pin 3 = RX, pin 2 = GND) at 115200 8N1.
# The Debug Probe carries a UART alongside CMSIS-DAP and enumerates it as a
# second CDC port, so one cable does both jobs: probe UART TX to J1 pin 3,
# probe UART RX to J1 pin 1, grounds common.
#
# Usage: ./console.sh [port] [baud]
#
# With no arguments it takes the single /dev/cu.usbmodem* it can find,
# which is the probe when nothing else is plugged in. Two of them is
# ambiguous rather than guessable, so it says so instead of picking.
#
set -euo pipefail

BAUD="${2:-115200}"
PORT="${1:-}"

if [[ -z "${PORT}" ]]; then
    # No mapfile: macOS still ships bash 3.2, and this script has to run
    # on the machine the board is plugged into.
    CANDIDATES=()
    for p in /dev/cu.usbmodem*; do
        [[ -e "$p" ]] && CANDIDATES+=("$p")
    done
    case "${#CANDIDATES[@]}" in
        0) echo "ERROR: no /dev/cu.usbmodem* found. Is the Debug Probe attached?" >&2
           exit 1 ;;
        1) PORT="${CANDIDATES[0]}" ;;
        *) echo "ERROR: more than one candidate port; name one:" >&2
           printf '  %s\n' "${CANDIDATES[@]}" >&2
           exit 1 ;;
    esac
fi

echo "Console: ${PORT} @ ${BAUD} (ctrl-a k to quit screen, ctrl-] for picocom)"

if command -v picocom >/dev/null 2>&1; then
    exec picocom -b "${BAUD}" "${PORT}"
elif command -v screen >/dev/null 2>&1; then
    exec screen "${PORT}" "${BAUD}"
else
    echo "Neither picocom nor screen is installed; falling back to cat." >&2
    stty -f "${PORT}" "${BAUD}" cs8 -cstopb -parenb
    exec cat "${PORT}"
fi
